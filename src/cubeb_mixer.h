/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef CUBEB_MIXER
#define CUBEB_MIXER

#include "cubeb/cubeb.h" // for cubeb_channel_layout and cubeb_stream_params.

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct cubeb_mixer cubeb_mixer;
cubeb_mixer * cubeb_mixer_create(cubeb_sample_format format,
                                 cubeb_channel_layout in_layout,
                                 cubeb_channel_layout out_layout);
void cubeb_mixer_destroy(cubeb_mixer * mixer);
int cubeb_mixer_mix(cubeb_mixer * mixer,
                    unsigned long frames,
                    void * input_buffer,
                    unsigned long input_buffer_size,
                    void * output_buffer,
                    unsigned long output_buffer_size);

unsigned int cubeb_channel_layout_nb_channels(cubeb_channel_layout channel_layout);

#if defined(__cplusplus)
}
#endif

#endif // CUBEB_MIXER
