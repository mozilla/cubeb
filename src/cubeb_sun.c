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
#include <stdio.h>
#include <string.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

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
  int floating;
  int play_fd;
  int record_fd;
  float volume;
  struct audio_info info;
};

int
sun_init(cubeb ** context, char const * context_name)
{
  *context = malloc(sizeof(*context));
  (*context)->ops = &sun_ops;
  (void)context_name;
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
 * XXX: hwprops is failing to detect a USB microphone as capture-only
 * currently, so I have to look for valid data instead. This is a bug,
 * and should be fixed as soon as possible.
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
  char dev[16] = "/dev/audio";
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
      prinfo = &hwfmt.record;
    }
    if ((hwprops & AUDIO_PROP_PLAYBACK) != 0 &&
        sun_prinfo_verify_sanity(&hwfmt.play)) {
      /* the device supports playback, probably */
      device.type |= CUBEB_DEVICE_TYPE_OUTPUT;
      prinfo = &hwfmt.play;
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
    return CUBEB_ERROR_NOT_SUPPORTED;
  }
  if (ioctl(fd, AUDIO_SETINFO, info) == -1) {
    return CUBEB_ERROR;
  }
  if (ioctl(fd, AUDIO_GETINFO, info) == -1) {
    return CUBEB_ERROR;
  }
  return CUBEB_OK;
}

static void
sun_stream_destroy(cubeb_stream * s)
{
  if (s->play_fd != -1) {
    close(s->play_fd);
  }
  if (s->play_fd != s->record_fd) {
    if (s->record_fd != -1) {
      close(s->record_fd);
    }
  }
  free(s);
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
  AUDIO_INITINFO(&s->info);
  if (input_device != NULL && output_device != NULL &&
      strcmp(input_device, output_device) == 0 &&
      input_stream_params != NULL && output_stream_params != NULL) {
    int fd;

    if ((fd = open(output_device, O_RDWR | O_NONBLOCK) == -1)) {
      LOG("Audio device cannot be opened as r/w");
      ret = CUBEB_ERROR;
      goto error;
    }
    s->play_fd = fd;
    s->record_fd = fd;
  }
  if (input_stream_params != NULL) {
    if (input_stream_params->prefs & CUBEB_STREAM_PREF_LOOPBACK) {
      LOG("Loopback not supported");
      ret = CUBEB_ERROR_NOT_SUPPORTED;
      goto error;
    }
    if (s->record_fd == -1 &&
       (s->record_fd = open(input_device, O_RDONLY | O_NONBLOCK) == -1)) { 
      LOG("Audio device cannot be opened as read-only");
      ret = CUBEB_ERROR;
      goto error;
    } 
    if ((ret = sun_copy_params(s->record_fd, s,
                        output_stream_params, &s->info.record)) != CUBEB_OK) {
      goto error;
    }
  }
  if (output_stream_params != NULL) {
    if (output_stream_params->prefs & CUBEB_STREAM_PREF_LOOPBACK) {
      LOG("Loopback not supported");
      ret = CUBEB_ERROR_NOT_SUPPORTED;
      goto error;
    }
    if (s->play_fd == -1 &&
       (s->play_fd = open(output_device, O_WRONLY | O_NONBLOCK) == -1)) {
      LOG("Audio device cannot be opened as write-only");
      ret = CUBEB_ERROR;
      goto error;
    }
    if ((ret = sun_copy_params(s->play_fd, s,
                        input_stream_params, &s->info.play)) != CUBEB_OK) {
      goto error;
    }
  }
  s->volume = 1.0;
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
  return CUBEB_OK;
}

static int
sun_stream_stop(cubeb_stream * s)
{
  return CUBEB_OK;
}

static int
sun_stream_get_position(cubeb_stream * stream, uint64_t * position)
{
  struct audio_offset ofs;
  unsigned channels;

  if (stream->play_fd != -1) {
    if (ioctl(stream->play_fd, AUDIO_GETOOFFS, &ofs) == -1) {
      return CUBEB_ERROR;
    }
    channels = stream->info.play.channels;
  } else {
    if (ioctl(stream->record_fd, AUDIO_GETIOFFS, &ofs) == -1) {
      return CUBEB_ERROR;
    }
    channels = stream->info.record.channels;
  }
  /* Convert bytes to frames */
  *position = ofs.samples / (channels * sizeof(uint16_t));
  return CUBEB_OK;
}

static int
sun_stream_get_latency(cubeb_stream * stream, uint32_t * latency)
{
  cubeb_stream_params params;

  params.rate = (stream->play_fd != -1) ?
    stream->info.play.sample_rate : stream->info.record.sample_rate;
  return sun_get_min_latency(NULL, params, latency);
}

static int
sun_stream_set_volume(cubeb_stream * stream, float volume)
{
  stream->volume = volume;
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
  .stream_get_current_device = NULL,
  .stream_device_destroy = NULL,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};
