/*
 * Copyright © 2013 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#include <stddef.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

struct cubeb {
  struct cubeb_ops * ops;
};

struct cubeb_stream {
  struct cubeb * context;
};

int
validate_stream_params(cubeb_stream_params stream_params)
{
  if (stream_params.rate < 1 || stream_params.rate > 192000 ||
      stream_params.channels < 1 || stream_params.channels > 8) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
  case CUBEB_SAMPLE_S16BE:
  case CUBEB_SAMPLE_FLOAT32LE:
  case CUBEB_SAMPLE_FLOAT32BE:
    return CUBEB_OK;
  }

  return CUBEB_ERROR_INVALID_FORMAT;
}

int
validate_latency(int latency)
{
  if (latency < 1 || latency > 2000) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }
  return CUBEB_OK;
}

int pulse_init(cubeb ** context, char const * context_name);

int
cubeb_init(cubeb ** context, char const * context_name)
{
  if (!context) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return pulse_init(context, context_name);
#if 0
  /* XXX: special sauce to create appropriate context for system's capabilities. */

  return (*context)->ops->init(context, context_name);
#endif
}

char const *
cubeb_get_backend_id(cubeb * context)
{
  if (!context) {
    return NULL;
  }

  return context->ops->get_backend_id(context);
}

void
cubeb_destroy(cubeb * context)
{
  if (!context) {
    return;
  }

  context->ops->destroy(context);
}

int
cubeb_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void * user_ptr)
{
  int r;

  if (!context || !stream) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  if ((r = validate_stream_params(stream_params)) != CUBEB_OK ||
      (r = validate_latency(latency)) != CUBEB_OK) {
    return r;
  }

  return context->ops->stream_init(context, stream, stream_name,
                                   stream_params, latency,
                                   data_callback,
                                   state_callback,
                                   user_ptr);
}

void
cubeb_stream_destroy(cubeb_stream * stream)
{
  if (!stream) {
    return;
  }

  stream->context->ops->stream_destroy(stream);
}

int
cubeb_stream_start(cubeb_stream * stream)
{
  if (!stream) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return stream->context->ops->stream_start(stream);
}

int
cubeb_stream_stop(cubeb_stream * stream)
{
  if (!stream) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return stream->context->ops->stream_stop(stream);
}

int
cubeb_stream_get_position(cubeb_stream * stream, uint64_t * position)
{
  if (!stream || !position) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return stream->context->ops->stream_get_position(stream, position);
}
