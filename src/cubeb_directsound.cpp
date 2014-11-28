/*
 * Copyright © 2011 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#define INITGUID
#include <assert.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <cguid.h>
#include <process.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#include <stdio.h>

struct cubeb_list_node {
  struct cubeb_list_node * next;
  struct cubeb_list_node * prev;
  struct cubeb_stream * stream;
};

static struct cubeb_ops const directsound_ops;

struct cubeb {
  struct cubeb_ops const * ops;
  HWND hidden_window;
  LPDIRECTSOUND dsound;
  HANDLE refill_thread;
  CRITICAL_SECTION lock;
  HANDLE streams_event;
  struct cubeb_list_node * streams;
  int shutdown;
};

struct cubeb_stream {
  cubeb * context;
  LPDIRECTSOUNDBUFFER buffer;
  DWORD buffer_size;
  cubeb_stream_params params;
  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void * user_ptr;
  unsigned long written;
  unsigned long slipped;
  DWORD last_refill;
  CRITICAL_SECTION lock;
  int active;
  int draining;
  struct cubeb_list_node * node;
};

static size_t
bytes_per_frame(cubeb_stream_params params)
{
  size_t bytes;

  switch (params.format) {
  case CUBEB_SAMPLE_S16LE:
    bytes = sizeof(signed short);
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    bytes = sizeof(float);
    break;
  default:
    assert(false);
  }

  return bytes * params.channels;
}

static void
refill_stream(cubeb_stream * stm, int prefill)
{
  VOID * p1, * p2;
  DWORD p1sz, p2sz;
  HRESULT r;
  long dt;

  /* calculate how much has played since last refill */
  DWORD play, write;
  r = stm->buffer->GetCurrentPosition(&play, &write);
  assert(r == DS_OK);

  long gap = write - play;
  if (gap < 0) {
    gap += stm->buffer_size;
  }

#if 1
  dt = GetTickCount() - stm->last_refill;
  if (!prefill) {
    double buflen = (double) (stm->buffer_size - gap) / bytes_per_frame(stm->params) / stm->params.rate * 1000;
    if (dt > buflen) {
      fprintf(stderr, "*** buffer wrap (%ld, %f, %f)***\n", dt, buflen, dt - buflen);
      stm->slipped += (dt - buflen) / 1000.0 * bytes_per_frame(stm->params) * stm->params.rate;
    }
  }
#endif

  unsigned long writepos = stm->written % stm->buffer_size;

  long playsz = 0;
  if (write < writepos) {
    playsz = write + stm->buffer_size - writepos;
  } else {
    playsz = write - writepos;
  }

  /* can't write between play and write cursors */
  playsz -= gap;
  if (playsz < 0) {
#if 0
    fprintf(stderr, "** negcapped, dt=%u real nwl=%ld p=%u w=%u g=%ld wo=%u **\n",
	    dt, playsz, play, write, gap, writepos);
#endif
    return;
  }

  if (prefill) {
    playsz = stm->buffer_size;
  }

  playsz -= bytes_per_frame(stm->params);

  /* no space to refill */
  if (playsz <= 0)
    return;

  /*assert(writepos >= write && ((writepos + playsz) % stm->buffer_size) < play);*/

  /*
    assumptions: buffer with w==p is full or empty
    we know total writes is stm->written
    so w==p and stm->written%stm->buffer_size==0 full or empty
    need abs play pos to determine
    rel play pos is (write + stm->buffer_size) - play
    (0 + 10) - 0 -> 10 -> also assumes buffer is full
    absplayed must be between stm->written-stm->buffer_size and stm->written.

    XXX want prefill logic to work anytime as we will eventually call it from start()
  */

  r = stm->buffer->Lock(writepos, playsz, &p1, &p1sz, &p2, &p2sz, 0);
  if (r == DSERR_BUFFERLOST) {
    stm->buffer->Restore();
    r = stm->buffer->Lock(writepos, playsz, &p1, &p1sz, &p2, &p2sz, 0);
  }
  assert(r == DS_OK);
  assert(p1sz % bytes_per_frame(stm->params) == 0);
  assert(p2sz % bytes_per_frame(stm->params) == 0);

  int r = stm->data_callback(stm, stm->user_ptr, p1,
			     p1sz / bytes_per_frame(stm->params));
  if (p2 && r == CUBEB_OK) {
    r = stm->data_callback(stm, stm->user_ptr, p2,
			   p2sz / bytes_per_frame(stm->params));
  } else {
    p2sz = 0;
  }

#if 0
  // XXX fix EOS/drain handling
  if (r == CUBEB_EOS) {
    LPDIRECTSOUNDNOTIFY notify;
    r = stm->buffer->QueryInterface(IID_IDirectSoundNotify, (LPVOID *) &notify);
    assert(r == DS_OK);

    DSBPOSITIONNOTIFY note;
    note.dwOffset = (writepos + p1sz + p2sz) % stm->buffer_size;
    note.hEventNotify = stm->context->streams_event;
    if (notify->SetNotificationPositions(1, &note) != DS_OK) {
      /* XXX free resources */
      assert(false);
    }

    notify->Release();
    stm->draining = 1;
  }
#endif

  stm->last_refill = GetTickCount();
  stm->written += p1sz + p2sz;
  r = stm->buffer->Unlock(p1, p1sz, p2, p2sz);
  assert(r == DS_OK);
}

unsigned __stdcall
directsound_buffer_refill_thread(void * user_ptr)
{
  int shutdown = 0;
  cubeb * ctx = (cubeb *) user_ptr;
  assert(ctx);

  while (!shutdown) {
    DWORD r = WaitForSingleObject(ctx->streams_event, INFINITE);
    assert(r == WAIT_OBJECT_0);
    EnterCriticalSection(&ctx->lock);
    struct cubeb_list_node * node = ctx->streams;
    while (node) {
      EnterCriticalSection(&node->stream->lock);
      if (node->stream->draining) {
	node->stream->state_callback(node->stream, node->stream->user_ptr, CUBEB_STATE_DRAINED);
	node->stream->active = 0;
	HRESULT r = node->stream->buffer->Stop();
	assert(r == DS_OK);
	DWORD play, write;
	r = node->stream->buffer->GetCurrentPosition(&play, &write);
	assert(r == DS_OK);
	fprintf(stderr, "stm %p drained (p=%u w=%u)\n", node->stream, play, write);
      }
      if (node->stream->active) {
	refill_stream(node->stream, 0);
      }
      LeaveCriticalSection(&node->stream->lock);
      node = node->next;
    }
    shutdown = ctx->shutdown;
    LeaveCriticalSection(&ctx->lock);
  }
  return 0;
}

static LRESULT CALLBACK
hidden_window_callback(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch(msg) {
  case WM_CLOSE:
    DestroyWindow(hwnd);
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    break;
  default:
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }
  return 0;
}

char const hidden_window_class_name[] = "cubeb_hidden_window_class";

static void directsound_destroy(cubeb * ctx);

/*static*/ int
directsound_init(cubeb ** context, char const * context_name)
{
  cubeb * ctx;

  *context = NULL;

  ctx = (cubeb *) calloc(1, sizeof(*ctx));

  ctx->ops = &directsound_ops;

  /* register a hidden window for DirectSound's SetCooperativeLevel */
  WNDCLASSEX wc;
  wc.cbSize        = sizeof(WNDCLASSEX);
  wc.style         = 0;
  wc.lpfnWndProc   = hidden_window_callback;
  wc.cbClsExtra    = 0;
  wc.cbWndExtra    = 0;
  wc.hInstance     = NULL;
  wc.hIcon         = NULL;
  wc.hCursor       = NULL;
  wc.hbrBackground = (HBRUSH) COLOR_WINDOW + 1;
  wc.lpszMenuName  = NULL;
  wc.lpszClassName = hidden_window_class_name;
  wc.hIconSm       = NULL;

  /* ignore the result as registration failure will show up when the
     window is created and this way there is no need for global
     tracking of whether the window class has been registered */
  RegisterClassEx(&wc);

  ctx->hidden_window = CreateWindowEx(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
				      hidden_window_class_name, NULL, WS_DISABLED,
				      0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
  if (!ctx->hidden_window) {
    directsound_destroy(ctx);
    return CUBEB_ERROR;
  }

  if (FAILED(DirectSoundCreate(NULL, &ctx->dsound, NULL))) {
    directsound_destroy(ctx);
    return CUBEB_ERROR;
  }
  assert(ctx->dsound);

  if (FAILED(ctx->dsound->SetCooperativeLevel(ctx->hidden_window, DSSCL_PRIORITY))) {
    directsound_destroy(ctx);
    return CUBEB_ERROR;
  }

  InitializeCriticalSection(&ctx->lock);

  ctx->streams_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!ctx->streams_event) {
    directsound_destroy(ctx);
    return CUBEB_ERROR;
  }

  uintptr_t thread = _beginthreadex(NULL, 64 * 1024,
				    cubeb_buffer_refill_thread, ctx, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!thread) {
    directsound_destroy(ctx);
    return CUBEB_ERROR;
  }
  ctx->refill_thread = reinterpret_cast<HANDLE>(thread);

  SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);

  *context = ctx;

  return CUBEB_OK;
}

static char const *
directsound_get_backend_id(cubeb * ctx)
{
  return "directsound";
}

static void
directsound_destroy(cubeb * ctx)
{
  assert(!ctx->streams);

  if (ctx->refill_thread) {
    EnterCriticalSection(&ctx->lock);
    ctx->shutdown = 1;
    LeaveCriticalSection(&ctx->lock);
    SetEvent(ctx->streams_event);
    HRESULT r = WaitForSingleObject(ctx->refill_thread, INFINITE);
    assert(r == WAIT_OBJECT_0);

    CloseHandle(ctx->refill_thread);
  }

  if (ctx->streams_event) {
    CloseHandle(ctx->streams_event);
  }

  DeleteCriticalSection(&ctx->lock);

  if (ctx->dsound) {
    ctx->dsound->Release();
  }

  if (ctx->hidden_window) {
    DestroyWindow(ctx->hidden_window);
  }

  free(ctx);
}

static int
directsound_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                        cubeb_stream_params stream_params, unsigned int latency,
                        cubeb_data_callback data_callback,
                        cubeb_state_callback state_callback,
                        void * user_ptr)
{
  struct cubeb_list_node * node;

  assert(context);
  *stream = NULL;

  /*
    create primary buffer
  */
  DSBUFFERDESC bd;
  bd.dwSize = sizeof(DSBUFFERDESC);
  bd.dwFlags = DSBCAPS_PRIMARYBUFFER;
  bd.dwBufferBytes = 0;
  bd.dwReserved = 0;
  bd.lpwfxFormat = NULL;
  bd.guid3DAlgorithm = DS3DALG_DEFAULT;

  LPDIRECTSOUNDBUFFER primary;
  if (FAILED(context->dsound->CreateSoundBuffer(&bd, &primary, NULL))) {
    return 1;
  }

  WAVEFORMATEXTENSIBLE wfx;
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = stream_params.channels;
  wfx.Format.nSamplesPerSec = stream_params.rate;
  wfx.Format.cbSize = sizeof(wfx) - sizeof(wfx.Format);

  /* XXX fix channel mappings */
  wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
    wfx.Format.wBitsPerSample = 16;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    break;
  case CUBEB_SAMPLE_FLOAT32LE:
    wfx.Format.wBitsPerSample = 32;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    break;
  default:
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  wfx.Format.nBlockAlign = (wfx.Format.wBitsPerSample * wfx.Format.nChannels) / 8;
  wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
  wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;

  if (FAILED(primary->SetFormat((LPWAVEFORMATEX) &wfx))) {
    /* XXX free primary */
    return CUBEB_ERROR;
  }

  primary->Release();

  cubeb_stream * stm = (cubeb_stream *) calloc(1, sizeof(*stm));
  assert(stm);

  stm->context = context;

  stm->params = stream_params;

  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->user_ptr = user_ptr;

  InitializeCriticalSection(&stm->lock);

  /*
    create secondary buffer
  */
  bd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPOSITIONNOTIFY;
  bd.dwBufferBytes = (DWORD) (wfx.Format.nSamplesPerSec / 1000.0 * latency * bytes_per_frame(stream_params));
  if (bd.dwBufferBytes % bytes_per_frame(stream_params) != 0) {
    bd.dwBufferBytes += bytes_per_frame(stream_params) - (bd.dwBufferBytes % bytes_per_frame(stream_params));
  }
  bd.lpwfxFormat = (LPWAVEFORMATEX) &wfx;
  if (FAILED(context->dsound->CreateSoundBuffer(&bd, &stm->buffer, NULL))) {
    return CUBEB_ERROR;
  }

  stm->buffer_size = bd.dwBufferBytes;

  LPDIRECTSOUNDNOTIFY notify;
  if (stm->buffer->QueryInterface(IID_IDirectSoundNotify, (LPVOID *) &notify) != DS_OK) {
    /* XXX free resources */
    return CUBEB_ERROR;
  }

  DSBPOSITIONNOTIFY note[3];
  for (int i = 0; i < 3; ++i) {
    note[i].dwOffset = (stm->buffer_size / 4) * i;
    note[i].hEventNotify = context->streams_event;
  }
  if (notify->SetNotificationPositions(3, note) != DS_OK) {
    /* XXX free resources */
    return CUBEB_ERROR;
  }

  notify->Release();

  refill_stream(stm, 1);
  /* XXX remove this, just a test that double refill does not overwrite existing data */
  refill_stream(stm, 0);
  uint64_t pos;
  cubeb_stream_get_position(stm, &pos);

  stm->node = (struct cubeb_list_node *) calloc(1, sizeof(*node));
  stm->node->stream = stm;

  EnterCriticalSection(&context->lock);
  if (!context->streams) {
    context->streams = stm->node;
  } else {
    node = context->streams;
    while (node->next) {
      node = node->next;
    }
    node->next = stm->node;
    stm->node->prev = node;
  }
  LeaveCriticalSection(&context->lock);

  SetEvent(context->streams_event);

  *stream = stm;

  return CUBEB_OK;
}

static void
directsound_stream_destroy(cubeb_stream * stm)
{
  EnterCriticalSection(&stm->context->lock);
  if (stm->node->prev) {
    stm->node->prev->next = stm->node->next;
  }
  if (stm->node->next) {
    stm->node->next->prev = stm->node->prev;
  }
  if (stm->context->streams == stm->node) {
    stm->context->streams = stm->node->next;
  }
  LeaveCriticalSection(&stm->context->lock);
  SetEvent(stm->context->streams_event);

  EnterCriticalSection(&stm->lock);
  stm->buffer->Release();
  free(stm->node);
  LeaveCriticalSection(&stm->lock);
  DeleteCriticalSection(&stm->lock);
  free(stm);
}

static int
directsound_stream_start(cubeb_stream * stm)
{
  EnterCriticalSection(&stm->lock);
  stm->active = 1;
  HRESULT r = stm->buffer->Play(0, 0, DSBPLAY_LOOPING);
  if (r == DSERR_BUFFERLOST) {
    stm->buffer->Restore();
    r = stm->buffer->Play(0, 0, DSBPLAY_LOOPING);
  }
  assert(r == DS_OK);
  DWORD play, write;
  r = stm->buffer->GetCurrentPosition(&play, &write);
  assert(r == DS_OK);
  LeaveCriticalSection(&stm->lock);
  fprintf(stderr, "stm %p started (p=%u w=%u)\n", stm, play, write);
  return CUBEB_OK;
}

static int
directsound_stream_stop(cubeb_stream * stm)
{
  EnterCriticalSection(&stm->lock);
  stm->active = 0;
  HRESULT r = stm->buffer->Stop();
  assert(r == DS_OK);
  DWORD play, write;
  r = stm->buffer->GetCurrentPosition(&play, &write);
  assert(r == DS_OK);
  fprintf(stderr, "stm %p stopped (p=%u w=%u)\n", stm, play, write);
  LeaveCriticalSection(&stm->lock);
  return CUBEB_OK;
}

static int
directsound_stream_get_position(cubeb_stream * stm, uint64_t * position)
{
  EnterCriticalSection(&stm->lock);

  DWORD play, write;
  HRESULT r = stm->buffer->GetCurrentPosition(&play, &write);
  assert(r == DS_OK);

  // XXX upper limit on position is stm->written,
  // XXX then adjust by overflow timer
  // XXX then adjust by play positiong97

  unsigned long writepos = stm->written % stm->buffer_size;

  long space = play - writepos;
  if (space <= 0) {
    space += stm->buffer_size;
  }
  if (!stm->active) {
    space = 0;
  }
  long delay = stm->buffer_size - space;

  double pos = (double) ((stm->written - delay) / bytes_per_frame(stm->params)) / (double) stm->params.rate * 1000.0;
#if 1
  fprintf(stderr, "w=%lu space=%ld delay=%ld pos=%.2f (p=%u w=%u)\n", stm->written, space, delay, pos, play, write);
#endif

  *position = (stm->written - delay) / bytes_per_frame(stm->params);

  LeaveCriticalSection(&stm->lock);

  return CUBEB_OK;
}

static struct cubeb_ops const directsound_ops = {
  /*.init =*/ directsound_init,
  /*.get_backend_id =*/ directsound_get_backend_id,
  /*.destroy =*/ directsound_destroy,
  /*.stream_init =*/ directsound_stream_init,
  /*.stream_destroy =*/ directsound_stream_destroy,
  /*.stream_start =*/ directsound_stream_start,
  /*.stream_stop =*/ directsound_stream_stop,
  /*.stream_get_position =*/ directsound_stream_get_position
};
