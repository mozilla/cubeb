/*
 * Copyright © 2019-2020 Nia Alarie <nia@NetBSD.org>
 * Copyright © 2020 Ka Ho Ng <khng300@gmail.com>
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#include <errno.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

/* Supported well by most hardware. */
#ifndef OSS_PREFER_RATE
#define OSS_PREFER_RATE (48000)
#endif

/* Standard acceptable minimum. */
#ifndef OSS_LATENCY_MS
#define OSS_LATENCY_MS (40)
#endif

#ifndef OSS_DEFAULT_DEVICE
#define OSS_DEFAULT_DEVICE "/dev/dsp"
#endif

#ifndef OSS_DEFAULT_MIXER
#define OSS_DEFAULT_MIXER "/dev/mixer"
#endif

#ifndef OSS_DEFAULT_NFRAMES
#define OSS_DEFAULT_NFRAMES (32)
#endif

/*
 * The current maximum number of channels supported
 * on FreeBSD is 8.
 *
 * Reference: FreeBSD 12.1-RELEASE
 */

#ifndef OSS_MAX_CHANNELS
# ifdef __FreeBSD__
#  define OSS_MAX_CHANNELS (8)
# else
#  define OSS_MAX_CHANNELS (2)
# endif
#endif

static struct cubeb_ops const oss_ops;

struct cubeb {
  struct cubeb_ops const * ops;
};

struct oss_stream {
  oss_devnode_t name;
  int fd;
  void * buf;

  struct stream_info {
    int channels;
    int sample_rate;
    int fmt;
    int precision;
  } info;

  unsigned int frame_size; /* precision in bytes * channels */
  unsigned int nfr; /* Number of frames allocated */
  bool floating;
};

struct cubeb_stream {
  struct cubeb * context;
  void * user_ptr;
  pthread_t thread;
  pthread_mutex_t mutex; /* protects running, volume, frames_written */
  bool running;
  float volume;
  struct oss_stream play;
  struct oss_stream record;
  cubeb_data_callback data_cb;
  cubeb_state_callback state_cb;
  uint64_t frames_written;
  uint64_t blocks_written;
};

int
oss_init(cubeb **context, char const *context_name) {
  cubeb * c;

  (void)context_name;
  if ((c = calloc(1, sizeof(cubeb))) == NULL) {
    return CUBEB_ERROR;
  }
  c->ops = &oss_ops;
  *context = c;
  return CUBEB_OK;
}

static void
oss_destroy(cubeb * context)
{
  free(context);
}

static char const *
oss_get_backend_id(cubeb * context)
{
  return "oss";
}

static int
oss_get_preferred_sample_rate(cubeb * context, uint32_t * rate)
{
  (void)context;

  *rate = OSS_PREFER_RATE;
  return CUBEB_OK;
}

static int
oss_get_max_channel_count(cubeb * context, uint32_t * max_channels)
{
  (void)context;

  *max_channels = OSS_MAX_CHANNELS;
  return CUBEB_OK;
}

static int
oss_get_min_latency(cubeb * context, cubeb_stream_params params,
                    uint32_t * latency_frames)
{
  (void)context;

  *latency_frames = OSS_LATENCY_MS * params.rate / 1000;
  return CUBEB_OK;
}

static void
oss_free_cubeb_device_info_strings(cubeb_device_info *cdi)
{
  free((char *)cdi->devid);
  free((char *)cdi->device_id);
  free((char *)cdi->friendly_name);
  free((char *)cdi->group_id);
  cdi->devid = NULL;
  cdi->device_id = NULL;
  cdi->friendly_name = NULL;
  cdi->group_id = NULL;
}

static int
oss_enumerate_devices(cubeb * context, cubeb_device_type type,
                      cubeb_device_collection * collection)
{
  oss_sysinfo si;
  int error, i;
  cubeb_device_info *devinfop = NULL;
  int collection_cnt = 0;
  int mixer_fd;

  mixer_fd = open(OSS_DEFAULT_MIXER, O_RDWR);
  if (mixer_fd == -1) {
    LOG("Failed to open mixer %s. errno: %d", OSS_DEFAULT_MIXER, errno);
    return CUBEB_ERROR;
  }

  error = ioctl(mixer_fd, SNDCTL_SYSINFO, &si);
  if (error)
    goto fail;

#if defined(__FreeBSD__)
  devinfop = calloc(si.numcards, sizeof(cubeb_device_info));
  if (devinfop == NULL)
    goto fail;

  /*
   * XXX: On FreeBSD we have to rely on SNDCTL_CARDINFO to get all
   * the usable audio devices currently, as SNDCTL_AUDIOINFO will
   * never return directly usable audio device nodes.
   */
  error = ioctl(mixer_fd, SNDCTL_CARDINFO, &si);
  if (error)
    return CUBEB_ERROR;

  for (i = 0; i < si.numcards; i++) {
    unsigned int devunit;
    oss_card_info ci;
    oss_audioinfo ai;
    oss_devnode_t dsppath;

    ci.card = i;
    error = ioctl(mixer_fd, SNDCTL_CARDINFO, &ci);
    if (error)
      continue;

    error = sscanf(ci.shortname, "pcm%u", &devunit);
    if (error < 1)
      continue;

    error = snprintf(dsppath, sizeof(dsppath), "/dev/dsp%u", devunit);
    if (error < 0)
      continue;

    int dspfd = open(dsppath, O_RDWR);
    if (dspfd == -1)
      continue;

 #define OSS_ENUM_CLOSECONT { close(dspfd); continue; }
    ai.dev = -1;
    error = ioctl(dspfd, SNDCTL_AUDIOINFO, &ai);
    if (error < 0)
      OSS_ENUM_CLOSECONT;

    devinfop[collection_cnt].type = 0;
    if (type & CUBEB_DEVICE_TYPE_INPUT) {
      if (!(ai.caps & DSP_CAP_INPUT))
        OSS_ENUM_CLOSECONT;
      devinfop[collection_cnt].type |= CUBEB_DEVICE_TYPE_INPUT;
    }
    if (type & CUBEB_DEVICE_TYPE_OUTPUT) {
      if (!(ai.caps & DSP_CAP_OUTPUT))
        OSS_ENUM_CLOSECONT;
      devinfop[collection_cnt].type |= CUBEB_DEVICE_TYPE_OUTPUT;
    }

    devinfop[collection_cnt].devid = strdup(dsppath);
    devinfop[collection_cnt].device_id = strdup(ci.shortname);
    devinfop[collection_cnt].friendly_name = strdup(ci.longname);
    devinfop[collection_cnt].group_id = strdup(ci.shortname);
    devinfop[collection_cnt].vendor_name = NULL;
    if (devinfop[collection_cnt].devid == NULL ||
        devinfop[collection_cnt].device_id == NULL ||
        devinfop[collection_cnt].friendly_name ||
        devinfop[collection_cnt].group_id) {
      oss_free_cubeb_device_info_strings(&devinfop[collection_cnt]);
      OSS_ENUM_CLOSECONT;
    }

    devinfop[collection_cnt].state = CUBEB_DEVICE_STATE_ENABLED;
    devinfop[collection_cnt].preferred = CUBEB_DEVICE_PREF_NONE;
    devinfop[collection_cnt].format = CUBEB_DEVICE_FMT_S16NE;
    devinfop[collection_cnt].default_format = CUBEB_DEVICE_FMT_S16NE;
    devinfop[collection_cnt].max_channels = ai.max_channels;
    devinfop[collection_cnt].default_rate = OSS_PREFER_RATE;
    devinfop[collection_cnt].max_rate = ai.max_rate;
    devinfop[collection_cnt].min_rate = ai.min_rate;
    devinfop[collection_cnt].latency_lo = 0;
    devinfop[collection_cnt].latency_hi = 0;
 #undef OSS_ENUM_CLOSECONT

    close(dspfd);
    collection_cnt++;
  }
#else
  devinfop = calloc(si.numaudios, sizeof(cubeb_device_info));
  if (devinfop == NULL)
    goto fail;

  collection->count = 0;
  for (i = 0; i < si.numaudios; i++) {
    oss_audioinfo ai;
    cubeb_device_info cdi = { 0 };

    ai.dev = i;
    error = ioctl(mixer_fd, SNDCTL_AUDIOINFO, &ai);
    if (error)
      goto fail;

    assert(ai.dev < si.numaudios);
    if (!ai.enabled)
      continue;

    if (type & CUBEB_DEVICE_TYPE_INPUT) {
      if (!(ai.caps & DSP_CAP_INPUT))
        continue;
      cdi.type |= CUBEB_DEVICE_TYPE_INPUT;
    }
    if (type & CUBEB_DEVICE_TYPE_OUTPUT) {
      if (!(ai.caps & DSP_CAP_OUTPUT))
        continue;
      cdi.type |= CUBEB_DEVICE_TYPE_OUTPUT;
    }

    cdi.devid = strdup(ai.devnode);
    cdi.device_id = strdup(ai.name);
    if (cdi.devid == NULL || cdi.device_id == NULL) {
      oss_free_cubeb_device_info_strings(&cdi);
      continue;
    }

    // TODO: Naming
    cdi.friendly_name = strdup(ai.name);
    cdi.group_id = strdup(ai.name);
    cdi.vendor_name = NULL;
    cdi.state = CUBEB_DEVICE_STATE_ENABLED;
    cdi.preferred = CUBEB_DEVICE_PREF_NONE;
    // TODO: More precise format detection
    cdi.format = CUBEB_DEVICE_FMT_S16NE;
    cdi.default_format = CUBEB_DEVICE_FMT_S16NE;
    cdi.max_channels = ai.max_channels;
    cdi.default_rate = OSS_PREFERRED_SAMPLE_RATE;
    cdi.max_rate = ai.max_rate;
    cdi.min_rate = ai.min_rate;
    cdi.latency_lo = oss_buf_size(1, MIN_LATENCY_PROFILE);
    cdi.latency_hi = oss_buf_size(1, MIN_LATENCY_PROFILE);

    devinfop[collection_cnt++] = cdi;
  }
#endif

  collection->count = collection_cnt;
  collection->device = devinfop;

  close(mixer_fd);
  return CUBEB_OK;

fail:
  close(mixer_fd);
  free(devinfop);
  return CUBEB_ERROR;
}

static int
oss_device_collection_destroy(cubeb * context,
                              cubeb_device_collection * collection)
{
  size_t i;
  for (i = 0; i < collection->count; i++) {
    oss_free_cubeb_device_info_strings(&collection->device[i]);
  }
  free(collection->device);
  collection->device = NULL;
  collection->count = 0;
  return 0;
}

static int
oss_copy_params(int fd, cubeb_stream * stream, cubeb_stream_params * params,
                struct stream_info * sinfo)
{
  sinfo->channels = params->channels;
  sinfo->sample_rate = params->rate;
  switch (params->format) {
  case CUBEB_SAMPLE_S16LE:
    sinfo->fmt = AFMT_S16_LE;
    sinfo->precision = 16;
    break;
  case CUBEB_SAMPLE_S16BE:
    sinfo->fmt = AFMT_S16_BE;
    sinfo->precision = 16;
    break;
  case CUBEB_SAMPLE_FLOAT32NE:
    sinfo->fmt = AFMT_S32_NE;
    sinfo->precision = 32;
    break;
  default:
    LOG("Unsupported format");
    return CUBEB_ERROR_INVALID_FORMAT;
  }
  if (ioctl(fd, SNDCTL_DSP_SETFMT, &sinfo->fmt) == -1) {
    return CUBEB_ERROR;
  }
  if (ioctl(fd, SNDCTL_DSP_CHANNELS, &sinfo->channels) == -1) {
    return CUBEB_ERROR;
  }
  if (ioctl(fd, SNDCTL_DSP_SPEED, &sinfo->sample_rate) == -1) {
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
oss_stream_stop(cubeb_stream * s)
{
  pthread_mutex_lock(&s->mutex);
  if (s->running) {
    s->running = false;
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread, NULL);
  } else {
    pthread_mutex_unlock(&s->mutex);
  }
  return CUBEB_OK;
}

static void
oss_stream_destroy(cubeb_stream * s)
{
  pthread_mutex_destroy(&s->mutex);
  oss_stream_stop(s);
  if (s->play.fd != -1) {
    close(s->play.fd);
  }
  if (s->record.fd != -1) {
    close(s->record.fd);
  }
  free(s->play.buf);
  free(s->record.buf);
  free(s);
}

static void
oss_float_to_linear32(void * buf, unsigned sample_count, float vol)
{
  float * in = buf;
  int32_t * out = buf;
  int32_t * tail = out + sample_count;

  while (out < tail) {
    float f = *(in++) * vol;
    if (f < -1.0)
      f = -1.0;
    else if (f > 1.0)
      f = 1.0;
    *(out++) = f * (float)INT32_MAX;
  }
}

static void
oss_linear32_to_float(void * buf, unsigned sample_count)
{
  int32_t * in = buf;
  float * out = buf;
  float * tail = out + sample_count;

  while (out < tail) {
    *(out++) = (1.0 / 0x80000000) * *(in++);
  }
}

static void
oss_linear16_set_vol(int16_t * buf, unsigned sample_count, float vol)
{
  unsigned i;
  int32_t multiplier = vol * 0x8000;

  for (i = 0; i < sample_count; ++i) {
    buf[i] = (buf[i] * multiplier) >> 15;
  }
}

static void *
oss_io_routine(void * arg)
{
  cubeb_stream *s = arg;
  cubeb_state state = CUBEB_STATE_STARTED;
  size_t to_read = 0;
  long to_write = 0;
  size_t write_ofs = 0;
  size_t read_ofs = 0;
  int drain = 0;

  s->state_cb(s, s->user_ptr, CUBEB_STATE_STARTED);
  while (state != CUBEB_STATE_ERROR) {
    pthread_mutex_lock(&s->mutex);
    if (!s->running) {
      pthread_mutex_unlock(&s->mutex);
      state = CUBEB_STATE_STOPPED;
      break;
    }
    pthread_mutex_unlock(&s->mutex);
    if (s->record.fd != -1 && s->record.floating) {
      oss_linear32_to_float(s->record.buf,
                            s->record.info.channels * s->record.nfr);
    }
    to_write = s->data_cb(s, s->user_ptr,
                          s->record.buf, s->play.buf,
                          s->play.fd != -1 ? s->play.nfr : s->record.nfr);
    if (to_write == CUBEB_ERROR) {
      state = CUBEB_STATE_ERROR;
      break;
    }
    if (s->play.fd != -1) {
      float vol;

      pthread_mutex_lock(&s->mutex);
      vol = s->volume;
      pthread_mutex_unlock(&s->mutex);

      if (s->play.floating) {
        oss_float_to_linear32(s->play.buf,
                              s->play.info.channels * to_write, vol);
      } else {
        oss_linear16_set_vol(s->play.buf,
                             s->play.info.channels * to_write, vol);
      }
    }
    if (to_write < s->play.nfr) {
      drain = 1;
    }
    to_write = s->play.fd != -1 ? to_write : 0;
    to_read = s->record.fd != -1 ? s->record.nfr : 0;
    write_ofs = 0;
    read_ofs = 0;
    while (to_write > 0 || to_read > 0) {
      size_t bytes;
      ssize_t n, frames;

      if (to_write > 0) {
        bytes = to_write * s->play.frame_size;
        if ((n = write(s->play.fd, (uint8_t *)s->play.buf + write_ofs, bytes)) < 0) {
          state = CUBEB_STATE_ERROR;
          break;
        }
        frames = n / s->play.frame_size;
        pthread_mutex_lock(&s->mutex);
        s->frames_written += frames;
        pthread_mutex_unlock(&s->mutex);
        to_write -= frames;
        write_ofs += n;
      }
      if (to_read > 0) {
        bytes = to_read * s->record.frame_size;
        if ((n = read(s->record.fd, (uint8_t *)s->record.buf + read_ofs, bytes)) < 0) {
          state = CUBEB_STATE_ERROR;
          break;
        }
        frames = n / s->record.frame_size;
        to_read -= frames;
        read_ofs += n;
      }
    }
    if (drain && state != CUBEB_STATE_ERROR) {
      state = CUBEB_STATE_DRAINED;
      break;
    }
  }
  s->state_cb(s, s->user_ptr, state);
  return NULL;
}

static int
oss_stream_init(cubeb * context,
                cubeb_stream ** stream,
                char const * stream_name,
                cubeb_devid input_device,
                cubeb_stream_params * input_stream_params,
                cubeb_devid output_device,
                cubeb_stream_params * output_stream_params,
                unsigned latency_frames,
                cubeb_data_callback data_callback,
                cubeb_state_callback state_callback,
                void * user_ptr)
{
  int ret = CUBEB_OK;
  cubeb_stream *s = NULL;

  (void)stream_name;
  (void)latency_frames;
  if ((s = calloc(1, sizeof(cubeb_stream))) == NULL) {
    ret = CUBEB_ERROR;
    goto error;
  }
  s->record.fd = -1;
  s->play.fd = -1;
  s->record.nfr = OSS_DEFAULT_NFRAMES;
  s->play.nfr = OSS_DEFAULT_NFRAMES;
  if (input_device != NULL) {
    strlcpy(s->record.name, input_device, sizeof(s->record.name));
  } else {
    strlcpy(s->record.name, OSS_DEFAULT_DEVICE, sizeof(s->record.name));
  }
  if (output_device != NULL) {
    strlcpy(s->play.name, output_device, sizeof(s->play.name));
  } else {
    strlcpy(s->play.name, OSS_DEFAULT_DEVICE, sizeof(s->play.name));
  }
  if (input_stream_params != NULL) {
    if (input_stream_params->prefs & CUBEB_STREAM_PREF_LOOPBACK) {
      LOG("Loopback not supported");
      ret = CUBEB_ERROR_NOT_SUPPORTED;
      goto error;
    }
    if (s->record.fd == -1) {
      if ((s->record.fd = open(s->record.name, O_RDONLY)) == -1) {
        LOG("Audio device could not be opened as read-only");
        ret = CUBEB_ERROR_DEVICE_UNAVAILABLE;
        goto error;
      }
    }
    if ((ret = oss_copy_params(s->record.fd, s, input_stream_params,
                               &s->record.info)) != CUBEB_OK) {
      LOG("Setting record params failed");
      goto error;
    }
    s->record.floating = (input_stream_params->format == CUBEB_SAMPLE_FLOAT32NE);
  }
  if (output_stream_params != NULL) {
    if (output_stream_params->prefs & CUBEB_STREAM_PREF_LOOPBACK) {
      LOG("Loopback not supported");
      ret = CUBEB_ERROR_NOT_SUPPORTED;
      goto error;
    }
    if (s->play.fd == -1) {
      if ((s->play.fd = open(s->play.name, O_WRONLY)) == -1) {
        LOG("Audio device could not be opened as write-only");
        ret = CUBEB_ERROR_DEVICE_UNAVAILABLE;
        goto error;
      }
    }
    if ((ret = oss_copy_params(s->play.fd, s, output_stream_params,
                               &s->play.info)) != CUBEB_OK) {
      LOG("Setting play params failed");
      goto error;
    }
    s->play.floating = (output_stream_params->format == CUBEB_SAMPLE_FLOAT32NE);
  }
  s->context = context;
  s->volume = 1.0;
  s->state_cb = state_callback;
  s->data_cb = data_callback;
  s->user_ptr = user_ptr;
  if (pthread_mutex_init(&s->mutex, NULL) != 0) {
    LOG("Failed to create mutex");
    goto error;
  }
  s->play.frame_size = s->play.info.channels *
                      (s->play.info.precision / 8);
  if (s->play.fd != -1) {
    audio_buf_info bi;
    if (ioctl(s->play.fd, SNDCTL_DSP_GETOSPACE, &bi) == 0)
      s->play.nfr = bi.bytes / s->play.frame_size;
    if ((s->play.buf = calloc(s->play.nfr, s->play.frame_size)) == NULL) {
      ret = CUBEB_ERROR;
      goto error;
    }
  }
  s->record.frame_size = s->record.info.channels *
                        (s->record.info.precision / 8);
  if (s->record.fd != -1) {
    audio_buf_info bi;
    if (ioctl(s->record.fd, SNDCTL_DSP_GETISPACE, &bi) == 0)
      s->record.nfr = bi.bytes / s->record.frame_size;
    if ((s->record.buf = calloc(s->record.nfr, s->record.frame_size)) == NULL) {
      ret = CUBEB_ERROR;
      goto error;
    }
  }
  *stream = s;
  return CUBEB_OK;
error:
  if (s != NULL) {
    oss_stream_destroy(s);
  }
  return ret;
}

static int
oss_stream_start(cubeb_stream * s)
{
  s->running = true;
  if (pthread_create(&s->thread, NULL, oss_io_routine, s) != 0) {
    LOG("Couldn't create thread");
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
oss_stream_get_position(cubeb_stream * s, uint64_t * position)
{
  pthread_mutex_lock(&s->mutex);
  *position = s->frames_written;
  pthread_mutex_unlock(&s->mutex);
  return CUBEB_OK;
}

static int
oss_stream_get_latency(cubeb_stream * s, uint32_t * latency)
{
  int delay;

  if (ioctl(s->play.fd, SNDCTL_DSP_GETODELAY, &delay) == -1) {
    return CUBEB_ERROR;
  }

  /* Return number of frames there */
  *latency = delay / s->play.frame_size;
  return CUBEB_OK;
}

static int
oss_stream_set_volume(cubeb_stream * stream, float volume)
{
  pthread_mutex_lock(&stream->mutex);
  stream->volume = volume;
  pthread_mutex_unlock(&stream->mutex);
  return CUBEB_OK;
}

static int
oss_get_current_device(cubeb_stream * stream, cubeb_device ** const device)
{
  *device = calloc(1, sizeof(cubeb_device));
  if (*device == NULL) {
    return CUBEB_ERROR;
  }
  (*device)->input_name = stream->record.fd != -1 ?
    strdup(stream->record.name) : NULL;
  (*device)->output_name = stream->play.fd != -1 ?
    strdup(stream->play.name) : NULL;
  return CUBEB_OK;
}

static int
oss_stream_device_destroy(cubeb_stream * stream, cubeb_device * device)
{
  (void)stream;
  free(device->input_name);
  free(device->output_name);
  free(device);
  return CUBEB_OK;
}

static struct cubeb_ops const oss_ops = {
    .init = oss_init,
    .get_backend_id = oss_get_backend_id,
    .get_max_channel_count = oss_get_max_channel_count,
    .get_min_latency = oss_get_min_latency,
    .get_preferred_sample_rate = oss_get_preferred_sample_rate,
    .enumerate_devices = oss_enumerate_devices,
    .device_collection_destroy = oss_device_collection_destroy,
    .destroy = oss_destroy,
    .stream_init = oss_stream_init,
    .stream_destroy = oss_stream_destroy,
    .stream_start = oss_stream_start,
    .stream_stop = oss_stream_stop,
    .stream_reset_default_device = NULL,
    .stream_get_position = oss_stream_get_position,
    .stream_get_latency = oss_stream_get_latency,
    .stream_get_input_latency = NULL,
    .stream_set_volume = oss_stream_set_volume,
    .stream_get_current_device = oss_get_current_device,
    .stream_device_destroy = oss_stream_device_destroy,
    .stream_register_device_changed_callback = NULL,
    .register_device_collection_changed = NULL};
