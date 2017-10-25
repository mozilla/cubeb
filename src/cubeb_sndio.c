/*
 * Copyright (c) 2011 Alexandre Ratchov <alex@caoua.org>
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sndio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#if defined(CUBEB_SNDIO_DEBUG)
#define DPR(...) fprintf(stderr, __VA_ARGS__);
#else
#define DPR(...) do {} while(0)
#endif

static struct cubeb_ops const sndio_ops;

struct cubeb {
  struct cubeb_ops const * ops;
};

struct cubeb_stream {
  cubeb * context;
  pthread_t th;                   /* to run real-time audio i/o */
  pthread_mutex_t mtx;            /* protects hdl and pos */
  struct sio_hdl *hdl;            /* link us to sndio */
  int active;                     /* cubec_start() called */
  int conv;                       /* need float->s16 conversion */
  unsigned char *pbuf;            /* play data is prepared here */
  unsigned int nfr;               /* number of frames in buf */
  unsigned int pbpf;              /* play bytes per frame */
  unsigned int pchan;             /* number of play channels */
  uint64_t hwpos;                 /* frame number Joe hears right now */
  uint64_t swpos;                 /* number of frames produced/consumed */
  cubeb_data_callback data_cb;    /* cb to preapare data */
  cubeb_state_callback state_cb;  /* cb to notify about state changes */
  void *arg;                      /* user arg to {data,state}_cb */
};

static void
float_to_s16(void *ptr, long nsamp)
{
  int16_t *dst = ptr;
  float *src = ptr;
  int s;

  while (nsamp-- > 0) {
    s = lrintf(*(src++) * 32768);
    if (s < -32768)
      s = -32768;
    else if (s > 32767)
      s = 32767;
    *(dst++) = s;
  }
}

static void
sndio_onmove(void *arg, int delta)
{
  cubeb_stream *s = (cubeb_stream *)arg;

  s->hwpos += delta;
}

static void *
sndio_mainloop(void *arg)
{
#define MAXFDS 8
  struct pollfd pfds[MAXFDS];
  cubeb_stream *s = arg;
  int n, nfds, revents, state = CUBEB_STATE_STARTED;
  size_t pstart = 0, pend = 0;
  long nfr;

  DPR("sndio_mainloop()\n");
  s->state_cb(s, s->arg, CUBEB_STATE_STARTED);
  pthread_mutex_lock(&s->mtx);
  if (!sio_start(s->hdl)) {
    pthread_mutex_unlock(&s->mtx);
    return NULL;
  }
  DPR("sndio_mainloop(), started\n");

  pstart = pend = s->nfr * s->pbpf;
  for (;;) {
    if (!s->active) {
      DPR("sndio_mainloop() stopped\n");
      state = CUBEB_STATE_STOPPED;
      break;
    }
    if (pstart == pend) {
      if (pend < s->nfr) {
        DPR("sndio_mainloop() drained\n");
        state = CUBEB_STATE_DRAINED;
        break;
      }
      pthread_mutex_unlock(&s->mtx);
      nfr = s->data_cb(s, s->arg, NULL, s->pbuf, s->nfr);
      pthread_mutex_lock(&s->mtx);
      if (nfr < 0) {
        DPR("sndio_mainloop() cb err\n");
        state = CUBEB_STATE_ERROR;
        break;
      }
      if (s->conv)
        float_to_s16(s->pbuf, nfr * s->pchan);
      s->swpos += nfr;
      pstart = 0;
      pend = nfr * s->pbpf;
    }
    if (pend == 0)
      continue;
    nfds = sio_pollfd(s->hdl, pfds, POLLOUT);
    if (nfds > 0) {
      pthread_mutex_unlock(&s->mtx);
      n = poll(pfds, nfds, -1);
      pthread_mutex_lock(&s->mtx);
      if (n < 0)
        continue;
    }
    revents = sio_revents(s->hdl, pfds);

    if (revents & POLLHUP) {
      state = CUBEB_STATE_ERROR;
      break;
    }

    if (revents & POLLOUT) {
      n = sio_write(s->hdl, s->pbuf + pstart, pend - pstart);
      if (n == 0 && sio_eof(s->hdl)) {
        DPR("sndio_mainloop() werr\n");
        state = CUBEB_STATE_ERROR;
        break;
      }
      pstart += n;
    }
  }
  sio_stop(s->hdl);
  s->hwpos = s->swpos;
  pthread_mutex_unlock(&s->mtx);
  s->state_cb(s, s->arg, state);
  return NULL;
}

/*static*/ int
sndio_init(cubeb **context, char const *context_name)
{
  DPR("sndio_init(%s)\n", context_name);
  *context = malloc(sizeof(*context));
  (*context)->ops = &sndio_ops;
  (void)context_name;
  return CUBEB_OK;
}

static char const *
sndio_get_backend_id(cubeb *context)
{
  return "sndio";
}

static void
sndio_destroy(cubeb *context)
{
  DPR("sndio_destroy()\n");
  free(context);
}

static int
sndio_stream_init(cubeb * context,
                  cubeb_stream ** stream,
                  char const * stream_name,
                  cubeb_devid input_device,
                  cubeb_stream_params * input_stream_params,
                  cubeb_devid output_device,
                  cubeb_stream_params * output_stream_params,
                  unsigned int latency_frames,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void *user_ptr)
{
  cubeb_stream *s;
  struct sio_par wpar, rpar;
  DPR("sndio_stream_init(%s)\n", stream_name);
  size_t size;

  assert(!input_stream_params && "not supported.");
  if (input_device || output_device) {
    /* Device selection not yet implemented. */
    return CUBEB_ERROR_DEVICE_UNAVAILABLE;
  }

  s = malloc(sizeof(cubeb_stream));
  if (s == NULL)
    return CUBEB_ERROR;
  s->context = context;
  s->hdl = sio_open(NULL, SIO_PLAY, 1);
  if (s->hdl == NULL) {
    free(s);
    DPR("sndio_stream_init(), sio_open() failed\n");
    return CUBEB_ERROR;
  }
  sio_initpar(&wpar);
  wpar.sig = 1;
  wpar.bits = 16;
  switch (output_stream_params->format) {
  case CUBEB_SAMPLE_S16LE:
    wpar.le = 1;
    break;
  case CUBEB_SAMPLE_S16BE:
    wpar.le = 0;
    break;
  case CUBEB_SAMPLE_FLOAT32NE:
    wpar.le = SIO_LE_NATIVE;
    break;
  default:
    sio_close(s->hdl);
    free(s);
    DPR("sndio_stream_init() unsupported format\n");
    return CUBEB_ERROR_INVALID_FORMAT;
  }
  wpar.rate = output_stream_params->rate;
  wpar.pchan = output_stream_params->channels;
  wpar.appbufsz = latency_frames;
  if (!sio_setpar(s->hdl, &wpar) || !sio_getpar(s->hdl, &rpar)) {
    sio_close(s->hdl);
    free(s);
    DPR("sndio_stream_init(), sio_setpar() failed\n");
    return CUBEB_ERROR;
  }
  if (rpar.bits != wpar.bits || rpar.le != wpar.le ||
      rpar.sig != wpar.sig || rpar.rate != wpar.rate ||
      rpar.pchan != wpar.pchan) {
    sio_close(s->hdl);
    free(s);
    DPR("sndio_stream_init() unsupported params\n");
    return CUBEB_ERROR_INVALID_FORMAT;
  }
  sio_onmove(s->hdl, sndio_onmove, s);
  s->active = 0;
  s->nfr = rpar.round;
  s->pbpf = rpar.bps * rpar.pchan;
  s->pchan = rpar.pchan;
  s->data_cb = data_callback;
  s->state_cb = state_callback;
  s->arg = user_ptr;
  s->mtx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
  s->hwpos = s->swpos = 0;
  if (output_stream_params->format == CUBEB_SAMPLE_FLOAT32LE) {
    s->conv = 1;
    size = rpar.round * rpar.pchan * sizeof(float);
  } else {
    s->conv = 0;
    size = rpar.round * rpar.pchan * rpar.bps;
  }
  s->pbuf = malloc(size);
  if (s->pbuf == NULL) {
    sio_close(s->hdl);
    free(s);
    return CUBEB_ERROR;
  }
  *stream = s;
  DPR("sndio_stream_init() end, ok\n");
  (void)context;
  (void)stream_name;
  return CUBEB_OK;
}

static int
sndio_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  assert(ctx && max_channels);

  *max_channels = 8;

  return CUBEB_OK;
}

static int
sndio_get_preferred_sample_rate(cubeb * ctx, uint32_t * rate)
{
  /*
   * We've no device-independent prefered rate; any rate will work if
   * sndiod is running. If it isn't, 48kHz is what is most likely to
   * work as most (but not all) devices support it.
   */
  *rate = 48000;
  return CUBEB_OK;
}

static int
sndio_get_min_latency(cubeb * ctx, cubeb_stream_params params, uint32_t * latency_frames)
{
  /*
   * We've no device-independent minimum latency.
   */
  *latency_frames = 2048;

  return CUBEB_OK;
}

static void
sndio_stream_destroy(cubeb_stream *s)
{
  DPR("sndio_stream_destroy()\n");
  sio_close(s->hdl);
  free(s);
}

static int
sndio_stream_start(cubeb_stream *s)
{
  int err;

  DPR("sndio_stream_start()\n");
  s->active = 1;
  err = pthread_create(&s->th, NULL, sndio_mainloop, s);
  if (err) {
    s->active = 0;
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
sndio_stream_stop(cubeb_stream *s)
{
  void *dummy;

  DPR("sndio_stream_stop()\n");
  if (s->active) {
    s->active = 0;
    pthread_join(s->th, &dummy);
  }
  return CUBEB_OK;
}

static int
sndio_stream_get_position(cubeb_stream *s, uint64_t *p)
{
  pthread_mutex_lock(&s->mtx);
  DPR("sndio_stream_get_position() %" PRId64 "\n", s->hwpos);
  *p = s->hwpos;
  pthread_mutex_unlock(&s->mtx);
  return CUBEB_OK;
}

static int
sndio_stream_set_volume(cubeb_stream *s, float volume)
{
  DPR("sndio_stream_set_volume(%f)\n", volume);
  pthread_mutex_lock(&s->mtx);
  sio_setvol(s->hdl, SIO_MAXVOL * volume);
  pthread_mutex_unlock(&s->mtx);
  return CUBEB_OK;
}

int
sndio_stream_get_latency(cubeb_stream * stm, uint32_t * latency)
{
  // http://www.openbsd.org/cgi-bin/man.cgi?query=sio_open
  // in the "Measuring the latency and buffers usage" paragraph.
  *latency = stm->swpos - stm->hwpos;
  return CUBEB_OK;
}

static int
sndio_enumerate_devices(cubeb *context, cubeb_device_type type,
	cubeb_device_collection *collection)
{
  static char dev[] = SIO_DEVANY;
  cubeb_device_info *device;

  if (type != CUBEB_DEVICE_TYPE_OUTPUT) {
    collection->count = 0;
    return CUBEB_OK;
  }

  device = malloc(sizeof(cubeb_device_info));
  if (device == NULL)
    return CUBEB_ERROR;

  device->devid = dev;		/* passed to stream_init() */
  device->device_id = dev;	/* printable in UI */
  device->friendly_name = dev;	/* same, but friendly */
  device->group_id = dev;	/* actual device if full-duplex */
  device->vendor_name = NULL;   /* may be NULL */
  device->type = type;		/* Input/Output */
  device->state = CUBEB_DEVICE_STATE_ENABLED;
  device->preferred = CUBEB_DEVICE_PREF_ALL;
  device->format = CUBEB_DEVICE_FMT_S16NE;
  device->default_format = CUBEB_DEVICE_FMT_S16NE;
  device->max_channels = 16;
  device->default_rate = 48000;
  device->min_rate = 4000;
  device->max_rate = 192000;
  device->latency_lo = 480;
  device->latency_hi = 9600;
  collection->device = device;
  collection->count = 1;
  return CUBEB_OK;
}

static int
sndio_device_collection_destroy(cubeb * context,
	cubeb_device_collection * collection)
{
  free(collection->device);
  return CUBEB_OK;
}

static struct cubeb_ops const sndio_ops = {
  .init = sndio_init,
  .get_backend_id = sndio_get_backend_id,
  .get_max_channel_count = sndio_get_max_channel_count,
  .get_min_latency = sndio_get_min_latency,
  .get_preferred_sample_rate = sndio_get_preferred_sample_rate,
  .get_preferred_channel_layout = NULL,
  .enumerate_devices = sndio_enumerate_devices,
  .device_collection_destroy = sndio_device_collection_destroy,
  .destroy = sndio_destroy,
  .stream_init = sndio_stream_init,
  .stream_destroy = sndio_stream_destroy,
  .stream_start = sndio_stream_start,
  .stream_stop = sndio_stream_stop,
  .stream_reset_default_device = NULL,
  .stream_get_position = sndio_stream_get_position,
  .stream_get_latency = sndio_stream_get_latency,
  .stream_set_volume = sndio_stream_set_volume,
  .stream_set_panning = NULL,
  .stream_get_current_device = NULL,
  .stream_device_destroy = NULL,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};
