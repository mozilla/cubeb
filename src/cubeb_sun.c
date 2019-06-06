/*
 * Copyright © 2019 Nia Alarie
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#include <sys/audioio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#define LOG(str) (fprintf(stderr, "%s\n", str))

#define BYTES_TO_FRAMES(bytes, channels) \
  (bytes / (channels * sizeof(int16_t)))

#define FRAMES_TO_BYTES(frames, channels) \
  (frames * (channels * sizeof(int16_t)))

/* Default to 4 + 1 for the default device. */
#ifndef SUN_DEVICE_COUNT
#define SUN_DEVICE_COUNT (5)
#endif

/* Supported well by most hardware. */
#ifndef SUN_PREFER_RATE
#define SUN_PREFER_RATE (48000)
#endif

/* Standard acceptable minimum. */
#ifndef SUN_LATENCY_MS
#define SUN_LATENCY_MS (40)
#endif

#ifndef SUN_DEFAULT_DEVICE
#define SUN_DEFAULT_DEVICE "/dev/audio"
#endif

#ifndef SUN_BUFSIZE
#define SUN_BUFSIZE (1024)
#endif

#ifndef SUN_POLL_TIMEOUT
#define SUN_POLL_TIMEOUT (1000)
#endif

/*
 * Supported on NetBSD regardless of hardware.
 */

#ifndef SUN_MAX_CHANNELS
#define SUN_MAX_CHANNELS (12)
#endif

#ifndef SUN_MIN_RATE
#define SUN_MIN_RATE (1000)
#endif

#ifndef SUN_MAX_RATE
#define SUN_MAX_RATE (192000)
#endif

static struct cubeb_ops const sun_ops;

struct cubeb {
  struct cubeb_ops const * ops;
};

struct cubeb_stream {
  struct cubeb * context;
  void * user_ptr;
  pthread_t thread;
  int floating;
  int running;
  int play_fd;
  int record_fd;
  float volume;
  struct audio_info p_info; /* info for the play fd */
  struct audio_info r_info; /* info for the record fd */
  cubeb_data_callback data_cb;
  cubeb_state_callback state_cb;
  int16_t * play_buf;
  int16_t * record_buf;
  float * f_play_buf;
  float * f_record_buf;
  char *input_name;
  char *output_name;
};

int
sun_init(cubeb ** context, char const * context_name)
{
  cubeb * c;

  (void)context_name;
  if ((c = calloc(1, sizeof(cubeb))) == NULL) {
    return CUBEB_ERROR;
  }
  c->ops = &sun_ops;
  *context = c;
  return CUBEB_OK;
}

static void
sun_destroy(cubeb * context)
{
  free(context);
}

static char const *
sun_get_backend_id(cubeb * context)
{
  return "sun";
}

static int
sun_get_preferred_sample_rate(cubeb * context, uint32_t * rate)
{
  (void)context;

  *rate = SUN_PREFER_RATE;
  return CUBEB_OK;
}

static int
sun_get_max_channel_count(cubeb * context, uint32_t * max_channels)
{
  (void)context;

  *max_channels = SUN_MAX_CHANNELS;
  return CUBEB_OK;
}

static int
sun_get_min_latency(cubeb * context, cubeb_stream_params params,
                    uint32_t * latency_frames)
{
  (void)context;

  *latency_frames = SUN_LATENCY_MS * params.rate / 1000;
  return CUBEB_OK;
}

static int
sun_get_hwinfo(const char * device, struct audio_info * format,
               int * props, struct audio_device * dev)
{
  int fd = -1;

  if ((fd = open(device, O_RDONLY)) == -1) {
    goto error;
  }
  if (ioctl(fd, AUDIO_GETFORMAT, format) != 0) {
    goto error;
  }
  if (ioctl(fd, AUDIO_GETPROPS, props) != 0) {
    goto error;
  }
  if (ioctl(fd, AUDIO_GETDEV, dev) != 0) {
    goto error;
  }
  close(fd);
  return CUBEB_OK;
error:
  if (fd != -1) {
    close(fd);
  }
  return CUBEB_ERROR;
}

/*
 * XXX: PR kern/54264
 */
static int
sun_prinfo_verify_sanity(struct audio_prinfo * prinfo)
{
   return prinfo->precision >= 8 && prinfo->precision <= 32 &&
     prinfo->channels >= 1 && prinfo->channels < SUN_MAX_CHANNELS &&
     prinfo->sample_rate < SUN_MAX_RATE && prinfo->sample_rate > SUN_MIN_RATE;
}

static int
sun_enumerate_devices(cubeb * context, cubeb_device_type type,
                      cubeb_device_collection * collection)
{
  unsigned i;
  cubeb_device_info device = {0};
  char dev[16] = SUN_DEFAULT_DEVICE;
  char dev_friendly[64];
  struct audio_info hwfmt;
  struct audio_device hwname;
  struct audio_prinfo *prinfo = NULL;
  int hwprops;

  collection->device = calloc(SUN_DEVICE_COUNT, sizeof(cubeb_device_info));
  if (collection->device == NULL) {
    return CUBEB_ERROR;
  }
  collection->count = 0;

  for (i = 0; i < SUN_DEVICE_COUNT; ++i) {
    if (i > 0) {
      (void)snprintf(dev, sizeof(dev), "/dev/audio%u", i - 1);
    }
    if (sun_get_hwinfo(dev, &hwfmt, &hwprops, &hwname) != CUBEB_OK) {
      continue;
    }
    device.type = 0;
    if ((hwprops & AUDIO_PROP_CAPTURE) != 0 &&
        sun_prinfo_verify_sanity(&hwfmt.record)) {
      /* the device supports recording, probably */
      device.type |= CUBEB_DEVICE_TYPE_INPUT;
    }
    if ((hwprops & AUDIO_PROP_PLAYBACK) != 0 &&
        sun_prinfo_verify_sanity(&hwfmt.play)) {
      /* the device supports playback, probably */
      device.type |= CUBEB_DEVICE_TYPE_OUTPUT;
    }
    switch (device.type) {
    case 0:
      /* device doesn't do input or output, aliens probably involved */
      continue;
    case CUBEB_DEVICE_TYPE_INPUT:
      if ((type & CUBEB_DEVICE_TYPE_INPUT) == 0) {
        /* this device is input only, not scanning for those, skip it */
        continue;
      }
      break;
    case CUBEB_DEVICE_TYPE_OUTPUT:
      if ((type & CUBEB_DEVICE_TYPE_OUTPUT) == 0) {
        /* this device is output only, not scanning for those, skip it */
        continue;
      }
      break;
    }
    if ((type & CUBEB_DEVICE_TYPE_INPUT) != 0) {
      prinfo = &hwfmt.record;
    }
    if ((type & CUBEB_DEVICE_TYPE_OUTPUT) != 0) {
      prinfo = &hwfmt.play;
    }
    if (i > 0) {
      (void)snprintf(dev_friendly, sizeof(dev_friendly), "%s %s %s (%d)",
                     hwname.name, hwname.version, hwname.config, i - 1);
    } else {
      (void)snprintf(dev_friendly, sizeof(dev_friendly), "%s %s %s (default)",
                     hwname.name, hwname.version, hwname.config);
    }
    device.devid = strdup(dev);
    device.device_id = strdup(dev);
    device.friendly_name = strdup(dev_friendly);
    device.group_id = strdup(dev);
    device.vendor_name = strdup(hwname.name);
    device.type = type;
    device.state = CUBEB_DEVICE_STATE_ENABLED;
    device.preferred = (i == 0) ? CUBEB_DEVICE_PREF_ALL : CUBEB_DEVICE_PREF_NONE;
    device.max_channels = prinfo->channels;
    device.default_rate = prinfo->sample_rate;
    device.default_format = CUBEB_DEVICE_FMT_S16NE;
    device.format = CUBEB_DEVICE_FMT_S16NE;
    device.min_rate = SUN_MIN_RATE;
    device.max_rate = SUN_MAX_RATE;
    device.latency_lo = SUN_LATENCY_MS * SUN_MIN_RATE / 1000;
    device.latency_hi = SUN_LATENCY_MS * SUN_MAX_RATE / 1000;
    collection->device[collection->count++] = device;
  }
  return CUBEB_OK;
}

static int
sun_device_collection_destroy(cubeb * context,
                              cubeb_device_collection * collection)
{
  unsigned i;

  for (i = 0; i < collection->count; ++i) {
    free((char *)collection->device[i].devid);
    free((char *)collection->device[i].device_id);
    free((char *)collection->device[i].friendly_name);
    free((char *)collection->device[i].group_id);
    free((char *)collection->device[i].vendor_name);
  }
  free(collection->device);
  return CUBEB_OK;
}

static int
sun_copy_params(int fd, cubeb_stream * stream, cubeb_stream_params * params,
                struct audio_prinfo * info)
{
  info->channels = params->channels;
  info->sample_rate = params->rate;
  info->precision = 16;
  switch (params->format) {
  case CUBEB_SAMPLE_S16LE:
    info->encoding = AUDIO_ENCODING_SLINEAR_LE;
    break;
  case CUBEB_SAMPLE_S16BE:
    info->encoding = AUDIO_ENCODING_SLINEAR_BE;
    break;
  case CUBEB_SAMPLE_FLOAT32NE:
    stream->floating = 1;
    info->encoding = AUDIO_ENCODING_SLINEAR;
    break;
  default:
    LOG("Unsupported format");
    return CUBEB_ERROR_INVALID_FORMAT;
  }
  if (ioctl(fd, AUDIO_SETINFO, info) == -1) {
    return CUBEB_ERROR;
  }
  if (ioctl(fd, AUDIO_GETINFO, info) == -1) {
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
sun_stream_stop(cubeb_stream * s)
{
  if (s->running) {
    s->running = 0;
    pthread_join(s->thread, NULL);
  }
  return CUBEB_OK;
}

static void
sun_stream_destroy(cubeb_stream * s)
{
  sun_stream_stop(s);
  if (s->play_fd != -1) {
    close(s->play_fd);
  }
  if (s->record_fd != -1) {
    close(s->record_fd);
  }
  free(s->f_play_buf);
  free(s->f_record_buf);
  free(s->play_buf);
  free(s->record_buf);
  free(s->output_name);
  free(s->input_name);
  free(s);
}

static void
sun_float_to_linear(float * in, int16_t * out,
                    unsigned channels, long frames, float vol)
{
  unsigned i, sample_count = frames * channels;
  float multiplier = vol * 0x8000;

  for (i = 0; i < sample_count; ++i) {
    int32_t sample = lrintf(in[i] * multiplier);
    if (sample < -0x8000) {
      out[i] = -0x8000;
    } else if (sample > 0x7fff) {
      out[i] = 0x7fff;
    } else {
      out[i] = sample;
    }
  }
}

static void
sun_linear_to_float(int16_t * in, float * out,
                    unsigned channels, long count)
{
  /* TODO */
}

static void
sun_linear_set_vol(int16_t * buf, unsigned channels, long frames, float vol)
{
  unsigned i, sample_count = frames * channels;
  int32_t multiplier = vol * 0x8000;

  for (i = 0; i < sample_count; ++i) {
    buf[i] = (buf[i] * multiplier) >> 15;
  }
}

static void *
sun_io_routine(void * arg)
{
  cubeb_stream *s = arg;
  cubeb_state state = CUBEB_STATE_STARTED;
  size_t to_read;
  long to_write;
  size_t write_ofs;
  size_t read_ofs;
  struct pollfd pfds[2];

  s->state_cb(s, s->user_ptr, CUBEB_STATE_STARTED);
  while (state != CUBEB_STATE_ERROR) {
    if (!s->running) {
      state = CUBEB_STATE_STOPPED;
      break;
    }
    if (s->floating) {
      if (s->record_fd != -1) {
        sun_linear_to_float(s->record_buf, s->f_record_buf,
                            s->r_info.record.channels, SUN_BUFSIZE);
      }
      to_write = s->data_cb(s, s->user_ptr,
                            s->f_record_buf, s->f_play_buf, SUN_BUFSIZE);
      if (to_write == CUBEB_ERROR) {
        state = CUBEB_STATE_ERROR;
        break;
      }
      if (s->play_fd != -1) {
        sun_float_to_linear(s->f_play_buf, s->play_buf,
                            s->p_info.play.channels, to_write, s->volume);
      }
    } else {
      to_write = s->data_cb(s, s->user_ptr,
                            s->record_buf, s->play_buf, SUN_BUFSIZE);
      if (to_write == CUBEB_ERROR) {
        state = CUBEB_STATE_ERROR;
        break;
      }
      if (s->play_fd != -1) {
        sun_linear_set_vol(s->play_buf, s->p_info.play.channels, to_write, s->volume);
      }
    }
    pfds[0].fd = s->play_fd;
    pfds[0].events = s->play_fd != -1 && to_write > 0 ? POLLOUT : 0;
    pfds[1].fd = s->record_fd;
    if (s->record_fd != -1) {
      pfds[1].events = POLLIN;
      to_read = SUN_BUFSIZE;
    } else {
      pfds[1].events = 0;
      to_read = 0;
    }
    write_ofs = 0;
    read_ofs = 0;
    do {
      size_t bytes;
      ssize_t n, frames;

      if (poll(pfds, 2, SUN_POLL_TIMEOUT) == -1) {
        LOG("poll failed");
        state = CUBEB_STATE_ERROR;
        break;
      }
      if ((pfds[0].revents & POLLHUP) || (pfds[1].revents & POLLHUP) ||
          (pfds[0].revents & POLLERR) || (pfds[1].revents & POLLERR)) {
        LOG("audio device disconnected");
        state = CUBEB_STATE_ERROR;
        break;
      }
      if (to_write > 0 && (pfds[0].revents & POLLOUT)) {
        bytes = FRAMES_TO_BYTES(to_write, s->p_info.play.channels);
        if ((n = write(s->play_fd, s->play_buf + write_ofs, bytes)) < 0) {
          state = CUBEB_STATE_ERROR;
          break;
        }
        frames = BYTES_TO_FRAMES(n, s->p_info.play.channels);
        to_write -= frames;
        write_ofs += frames;
        if (to_write == 0) {
          pfds[0].events = 0;
          s->state_cb(s, s->user_ptr, CUBEB_STATE_DRAINED);
        }
      }
      if (to_read > 0 && (pfds[1].revents & POLLIN)) {
        bytes = FRAMES_TO_BYTES(to_read, s->r_info.record.channels);
        if ((n = read(s->record_fd, s->record_buf + read_ofs, bytes)) < 0) {
          state = CUBEB_STATE_ERROR;
          break;
        }
        frames = BYTES_TO_FRAMES(n, s->r_info.record.channels);
        to_read -= frames;
        read_ofs += frames;
        if (to_read == 0) {
          pfds[1].events = 0;
        }
      }
    } while (to_write > 0 || to_read > 0);
  }
  s->state_cb(s, s->user_ptr, state);
  return NULL;
}

static int
sun_stream_init(cubeb * context,
                cubeb_stream ** stream,
                char const * stream_name,
                cubeb_devid input_device,
                cubeb_stream_params * input_stream_params,
                cubeb_devid output_device,
                cubeb_stream_params * output_stream_params,
                unsigned int latency,
                cubeb_data_callback data_callback,
                cubeb_state_callback state_callback,
                void * user_ptr)
{
  int ret = CUBEB_OK;
  cubeb_stream *s = NULL;

  (void)latency;
  (void)stream_name;
  if ((s = calloc(1, sizeof(cubeb_stream))) == NULL) {
    ret = CUBEB_ERROR;
    goto error;
  }
  s->record_fd = -1;
  s->play_fd = -1;
  if (input_device == NULL) {
    input_device = SUN_DEFAULT_DEVICE;
  }
  if (output_device == NULL) {
    output_device = SUN_DEFAULT_DEVICE;
  }
  if (input_stream_params != NULL) {
    if (input_stream_params->prefs & CUBEB_STREAM_PREF_LOOPBACK) {
      LOG("Loopback not supported");
      ret = CUBEB_ERROR_NOT_SUPPORTED;
      goto error;
    }
    if (s->record_fd == -1) {
      if ((s->record_fd = open(input_device, O_RDONLY | O_NONBLOCK)) == -1) {
        LOG("Audio device cannot be opened as read-only");
        ret = CUBEB_ERROR_DEVICE_UNAVAILABLE;
        goto error;
      }
    }
    AUDIO_INITINFO(&s->r_info);
    if ((ret = sun_copy_params(s->record_fd, s, input_stream_params,
                               &s->r_info.record)) != CUBEB_OK) {
      LOG("Setting record params failed");
      goto error;
    }
    s->input_name = strdup(input_device);
  }
  if (output_stream_params != NULL) {
    if (output_stream_params->prefs & CUBEB_STREAM_PREF_LOOPBACK) {
      LOG("Loopback not supported");
      ret = CUBEB_ERROR_NOT_SUPPORTED;
      goto error;
    }
    if (s->play_fd == -1) {
      if ((s->play_fd = open(output_device, O_WRONLY | O_NONBLOCK)) == -1) {
        LOG("Audio device cannot be opened as write-only");
        ret = CUBEB_ERROR_DEVICE_UNAVAILABLE;
        goto error;
      }
    }
    AUDIO_INITINFO(&s->p_info);
    if ((ret = sun_copy_params(s->play_fd, s, output_stream_params,
                               &s->p_info.play)) != CUBEB_OK) {
      LOG("Setting play params failed");
      goto error;
    }
    s->output_name = strdup(output_device);
  }
  s->context = context;
  s->volume = 1.0;
  s->state_cb = state_callback;
  s->data_cb = data_callback;
  s->user_ptr = user_ptr;
  if (s->play_fd != -1 && (s->play_buf = calloc(SUN_BUFSIZE,
      s->r_info.play.channels * sizeof(int16_t))) == NULL) {
    ret = CUBEB_ERROR;
    goto error;
  }
  if (s->record_fd != -1 && (s->record_buf = calloc(SUN_BUFSIZE,
      s->r_info.record.channels * sizeof(int16_t))) == NULL) {
    ret = CUBEB_ERROR;
    goto error;
  }
  if (s->floating) {
    if (s->play_fd != -1 && (s->f_play_buf = calloc(SUN_BUFSIZE,
        s->p_info.play.channels * sizeof(float))) == NULL) {
      ret = CUBEB_ERROR;
      goto error;
    }
    if (s->record_fd != -1 && (s->f_record_buf = calloc(SUN_BUFSIZE,
        s->r_info.record.channels * sizeof(float))) == NULL) {
      ret = CUBEB_ERROR;
      goto error;
    }
  }
  *stream = s;
  return CUBEB_OK;
error:
  if (s != NULL) {
    sun_stream_destroy(s);
  }
  return ret;
}

static int
sun_stream_start(cubeb_stream * s)
{
  s->running = 1;
  if (pthread_create(&s->thread, NULL, sun_io_routine, s) == -1) {
    LOG("Couldn't create thread");
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static int
sun_stream_get_position(cubeb_stream * s, uint64_t * position)
{
  struct audio_offset ofs;

  if (s->play_fd == -1 || ioctl(s->play_fd, AUDIO_GETOOFFS, &ofs) == -1) {
    return CUBEB_ERROR;
  }
  *position = BYTES_TO_FRAMES(ofs.samples, s->p_info.play.channels);
  return CUBEB_OK;
}

static int
sun_stream_get_latency(cubeb_stream * stream, uint32_t * latency)
{
  cubeb_stream_params params;

  params.rate = (stream->play_fd != -1) ?
    stream->p_info.play.sample_rate :
    stream->r_info.record.sample_rate;
  return sun_get_min_latency(NULL, params, latency);
}

static int
sun_stream_set_volume(cubeb_stream * stream, float volume)
{
  stream->volume = volume;
  return CUBEB_OK;
}

static int
sun_get_current_device(cubeb_stream * stream, cubeb_device ** const device)
{
  *device = calloc(1, sizeof(cubeb_device));
  if (*device == NULL) {
    return CUBEB_ERROR;
  }
  (*device)->input_name = stream->input_name != NULL ?
    strdup(stream->input_name) : NULL;
  (*device)->output_name = stream->output_name != NULL ?
    strdup(stream->output_name) : NULL;
  return CUBEB_OK;
}

static int
sun_stream_device_destroy(cubeb_stream * stream, cubeb_device * device)
{
  (void)stream;
  free(device->input_name);
  free(device->output_name);
  free(device);
  return CUBEB_OK;
}

static struct cubeb_ops const sun_ops = {
  .init = sun_init,
  .get_backend_id = sun_get_backend_id,
  .get_max_channel_count = sun_get_max_channel_count,
  .get_min_latency = sun_get_min_latency,
  .get_preferred_sample_rate = sun_get_preferred_sample_rate,
  .enumerate_devices = sun_enumerate_devices,
  .device_collection_destroy = sun_device_collection_destroy,
  .destroy = sun_destroy,
  .stream_init = sun_stream_init,
  .stream_destroy = sun_stream_destroy,
  .stream_start = sun_stream_start,
  .stream_stop = sun_stream_stop,
  .stream_reset_default_device = NULL,
  .stream_get_position = sun_stream_get_position,
  .stream_get_latency = sun_stream_get_latency,
  .stream_set_volume = sun_stream_set_volume,
  .stream_set_panning = NULL,
  .stream_get_current_device = sun_get_current_device,
  .stream_device_destroy = sun_stream_device_destroy,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};
