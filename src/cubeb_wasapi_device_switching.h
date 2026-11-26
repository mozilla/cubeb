/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license. See the
 * accompanying file LICENSE for details.
 */
#ifndef CUBEB_WASAPI_DEVICE_SWITCHING_H
#define CUBEB_WASAPI_DEVICE_SWITCHING_H

#include "cubeb/cubeb.h"

class wasapi_device_switching_policy {
public:
  wasapi_device_switching_policy(void const * input_device,
                                 cubeb_stream_params const * input_params,
                                 void const * output_device,
                                 cubeb_stream_params const * output_params)
      : follow_input(follows_default(input_device, input_params)),
        follow_output(follows_default(output_device, output_params)),
        input_flow(input_params &&
                           (input_params->prefs & CUBEB_STREAM_PREF_LOOPBACK)
                       ? CUBEB_DEVICE_TYPE_OUTPUT
                       : CUBEB_DEVICE_TYPE_INPUT),
        input_voice(input_params &&
                    (input_params->prefs & CUBEB_STREAM_PREF_VOICE)),
        output_voice(output_params &&
                     (output_params->prefs & CUBEB_STREAM_PREF_VOICE))
  {
  }

  bool follows_any_default() const { return follow_input || follow_output; }

  bool should_switch(cubeb_device_type flow, bool voice) const
  {
    return (follow_input && flow == input_flow && voice == input_voice) ||
           (follow_output && flow == CUBEB_DEVICE_TYPE_OUTPUT &&
            voice == output_voice);
  }

private:
  static bool follows_default(void const * device,
                              cubeb_stream_params const * params)
  {
    return !device && params &&
           !(params->prefs & CUBEB_STREAM_PREF_DISABLE_DEVICE_SWITCHING);
  }

  bool follow_input;
  bool follow_output;
  cubeb_device_type input_flow;
  bool input_voice;
  bool output_voice;
};

#endif
