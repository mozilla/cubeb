/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 *
 * Adapted from code based on libswresample's rematrix.c
 * Copyright (C) 2011-2012 Michael Niedermayer (michaelni@gmx.at)
 *
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <climits>
#include <memory>
#include <type_traits>

#include "cubeb-internal.h"
#include "cubeb_mixer.h"
#include "cubeb_utils.h"

#ifndef FF_ARRAY_ELEMS
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define CHANNELS_MAX 32
#define FRONT_LEFT             0
#define FRONT_RIGHT            1
#define FRONT_CENTER           2
#define LOW_FREQUENCY          3
#define BACK_LEFT              4
#define BACK_RIGHT             5
#define FRONT_LEFT_OF_CENTER   6
#define FRONT_RIGHT_OF_CENTER  7
#define BACK_CENTER            8
#define SIDE_LEFT              9
#define SIDE_RIGHT             10
#define TOP_CENTER             11
#define TOP_FRONT_LEFT         12
#define TOP_FRONT_CENTER       13
#define TOP_FRONT_RIGHT        14
#define TOP_BACK_LEFT          15
#define TOP_BACK_CENTER        16
#define TOP_BACK_RIGHT         17
#define NUM_NAMED_CHANNELS     18

#ifndef M_SQRT1_2
#define M_SQRT1_2      0.70710678118654752440  /* 1/sqrt(2) */
#endif
#ifndef M_SQRT2
#define M_SQRT2        1.41421356237309504880  /* sqrt(2) */
#endif
#define SQRT3_2      1.22474487139158904909  /* sqrt(3/2) */

#define  C30DB  M_SQRT2
#define  C15DB  1.189207115
#define C__0DB  1.0
#define C_15DB  0.840896415
#define C_30DB  M_SQRT1_2
#define C_45DB  0.594603558
#define C_60DB  0.5

enum AVMatrixEncoding {
    AV_MATRIX_ENCODING_NONE,
    AV_MATRIX_ENCODING_DOLBY,
    AV_MATRIX_ENCODING_DPLII,
};

unsigned int cubeb_channel_layout_nb_channels(cubeb_channel_layout x)
{
#if __GNUC__ || __clang__
  return __builtin_popcount (x);
#else
  x -= (x >> 1) & 0x55555555;
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
  x = (x + (x >> 4)) & 0x0F0F0F0F;
  x += x >> 8;
  return (x + (x >> 16)) & 0x3F;
#endif
}

struct MixerContext {
  MixerContext(cubeb_sample_format f,
               cubeb_channel_layout in,
               cubeb_channel_layout out)
    : _format(f)
    , _in_ch_layout(in)
    , _out_ch_layout(out)
    , _in_ch_count(cubeb_channel_layout_nb_channels(in))
    , _out_ch_count(cubeb_channel_layout_nb_channels(out))
  {
    _valid = init() >= 0;
  }

  static bool even(cubeb_channel_layout layout)
  {
    if (!layout) {
      return true;
    }
    if (layout & (layout - 1)) {
      return true;
    }
    return false;
  }

  static cubeb_channel_layout clean_layout(cubeb_channel_layout layout)
  {
    if (layout && layout != CHANNEL_FRONT_LEFT && !(layout & (layout - 1))) {
      LOG("Treating layout as mono");
      return CHANNEL_FRONT_CENTER;
    }

    return layout;
  }

  static bool sane_layout(cubeb_channel_layout layout)
  {
    if (!(layout & AV_CH_LAYOUT_SURROUND)) { // at least 1 front speaker
      return false;
    }
    if (!even(layout & (CHANNEL_FRONT_LEFT |
                        CHANNEL_FRONT_RIGHT))) { // no asymetric front
      return false;
    }
    if (!even(layout &
              (CHANNEL_SIDE_LEFT | CHANNEL_SIDE_RIGHT))) { // no asymetric side
      return false;
    }
    if (!even(layout & (CHANNEL_BACK_LEFT | CHANNEL_BACK_RIGHT))) {
      return false;
    }
    if (!even(layout &
              (CHANNEL_FRONT_LEFT_OF_CENTER | CHANNEL_FRONT_RIGHT_OF_CENTER))) {
      return false;
    }
    if (cubeb_channel_layout_nb_channels(layout) >= CHANNELS_MAX) {
      return false;
    }
    return true;
  }

  int auto_matrix();
  int init();

  cubeb_sample_format _format;
  cubeb_channel_layout _in_ch_layout;              ///< input channel layout
  cubeb_channel_layout _out_ch_layout;             ///< output channel layout
  uint32_t _in_ch_count;                           ///< input channel count
  uint32_t _out_ch_count;                          ///< output channel count
  float _surround_mix_level = C_30DB;              ///< surround mixing level
  float _center_mix_level = C_30DB;                ///< center mixing level
  float _lfe_mix_level = 1;                        ///< LFE mixing level
  float _rematrix_volume = 1;                      ///< rematrixing volume coefficient
  AVMatrixEncoding _matrix_encoding = AV_MATRIX_ENCODING_NONE; ///< matrixed stereo encoding
  double _matrix[CHANNELS_MAX][CHANNELS_MAX] = {{ 0 }};        ///< floating point rematrixing coefficients
  float _matrix_flt[CHANNELS_MAX][CHANNELS_MAX] = {{ 0 }};     ///< single precision floating point rematrixing coefficients
  int32_t _matrix32[CHANNELS_MAX][CHANNELS_MAX] = {{ 0 }};     ///< 17.15 fixed point rematrixing coefficients
  uint8_t _matrix_ch[CHANNELS_MAX][CHANNELS_MAX+1] = {{ 0 }};  ///< Lists of input channels per output channel that have non zero rematrixing coefficients
  bool _clipping = false;                          ///< Set to true if clipping detection is required
  bool _valid = false;                             ///< Set to true if context is valid.
};

/**
 * Clip a signed integer value into the -32768,32767 range.
 * @param a value to clip
 * @return clipped value
 */
inline __attribute__((const)) int16_t av_clip_int16_c(int a)
{
  if ((a + 0x8000U) & ~0xFFFF)
    return (a >> 31) ^ 0x7FFF;
  else
    return a;
}

#define R_clip(x) av_clip_int16_c(((x) + 16384) >> 15)
#define R(x) (((x) + 16384) >> 15)

template<typename SAMPLE, typename COEFF, bool CLIP = false>
void
sum2(SAMPLE * out,
     uint32_t stride_out,
     const SAMPLE * in1,
     const SAMPLE * in2,
     uint32_t stride_in,
     COEFF coeff1,
     COEFF coeff2,
     uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) {
    *out = coeff1 * *in1 + coeff2 * *in2;
    out += stride_out;
    in1 += stride_in;
    in2 += stride_in;
  }
}

template<>
void
sum2<int16_t, int, true>(int16_t * out,
                         uint32_t stride_out,
                         const int16_t * in1,
                         const int16_t * in2,
                         uint32_t stride_in,
                         int coeff1,
                         int coeff2,
                         uint32_t frames)
{
  // Specialisation with clipping
  for (uint32_t i = 0; i < frames; i++) {
    *out = R_clip(coeff1 * *in1 + coeff2 * *in2);
    out += stride_out;
    in1 += stride_in;
    in2 += stride_in;
  }
}

template<>
void
sum2<int16_t, int, false>(int16_t * out,
                          uint32_t stride_out,
                          const int16_t * in1,
                          const int16_t * in2,
                          uint32_t stride_in,
                          int coeff1,
                          int coeff2,
                          uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) {
    *out = R(coeff1 * *in1 + coeff2 * *in2);
    out += stride_out;
    in1 += stride_in;
    in2 += stride_in;
  }
}

template<typename SAMPLE, typename COEFF, bool CLIP = false>
void
copy(SAMPLE * out,
     uint32_t stride_out,
     const SAMPLE * in,
     uint32_t stride_in,
     COEFF coeff,
     uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) {
    *out = coeff * *in;
    out += stride_out;
    in += stride_in;
  }
}

template<>
void
copy<int16_t, int, true>(int16_t * out,
                         uint32_t stride_out,
                         const int16_t * in,
                         uint32_t stride_in,
                         int coeff,
                         uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) {
    *out = R_clip(coeff * *in);
    out += stride_out;
    in += stride_in;
  }
}

template<>
void
copy<int16_t, int, false>(int16_t * out,
                          uint32_t stride_out,
                          const int16_t * in,
                          uint32_t stride_in,
                          int coeff,
                          uint32_t frames)
{
  for (uint32_t i = 0; i < frames; i++) {
    *out = R(coeff * *in);
    out += stride_out;
    in += stride_in;
  }
}

int MixerContext::auto_matrix()
{
  double matrix[NUM_NAMED_CHANNELS][NUM_NAMED_CHANNELS] = { { 0 } };
  cubeb_channel_layout unaccounted;
  double maxcoef = 0;
  float maxval;

  cubeb_channel_layout in_ch_layout = clean_layout(_in_ch_layout);
  cubeb_channel_layout out_ch_layout = clean_layout(_out_ch_layout);

  if (!sane_layout(in_ch_layout)) {
    // Channel Not Supported
    LOG("Input Layout %x is not supported", _in_ch_layout);
    return -1;
  }

  if (!sane_layout(out_ch_layout)) {
    LOG("Output Layout %x is not supported", _out_ch_layout);
    return -1;
  }

  for (uint32_t i = 0; i < FF_ARRAY_ELEMS(matrix); i++) {
    if (in_ch_layout & out_ch_layout & (1U << i)) {
      matrix[i][i] = 1.0;
    }
  }

  unaccounted = in_ch_layout & ~out_ch_layout;

  if (unaccounted & CHANNEL_FRONT_CENTER) {
    if ((out_ch_layout & AV_CH_LAYOUT_STEREO) == AV_CH_LAYOUT_STEREO) {
      if (in_ch_layout & AV_CH_LAYOUT_STEREO) {
        matrix[FRONT_LEFT][FRONT_CENTER] += _center_mix_level;
        matrix[FRONT_RIGHT][FRONT_CENTER] += _center_mix_level;
      } else {
        matrix[FRONT_LEFT][FRONT_CENTER] += M_SQRT1_2;
        matrix[FRONT_RIGHT][FRONT_CENTER] += M_SQRT1_2;
      }
    } else {
      assert(false);
    }
  }
  if (unaccounted & AV_CH_LAYOUT_STEREO) {
    if (out_ch_layout & CHANNEL_FRONT_CENTER) {
      matrix[FRONT_CENTER][FRONT_LEFT] += M_SQRT1_2;
      matrix[FRONT_CENTER][FRONT_RIGHT] += M_SQRT1_2;
      if (in_ch_layout & CHANNEL_FRONT_CENTER)
        matrix[FRONT_CENTER][FRONT_CENTER] =
          _center_mix_level * M_SQRT2;
    } else {
      assert(false);
    }
  }

  if (unaccounted & CHANNEL_BACK_CENTER) {
    if (out_ch_layout & CHANNEL_BACK_LEFT) {
      matrix[BACK_LEFT][BACK_CENTER] += M_SQRT1_2;
      matrix[BACK_RIGHT][BACK_CENTER] += M_SQRT1_2;
    } else if (out_ch_layout & CHANNEL_SIDE_LEFT) {
      matrix[SIDE_LEFT][BACK_CENTER] += M_SQRT1_2;
      matrix[SIDE_RIGHT][BACK_CENTER] += M_SQRT1_2;
    } else if (out_ch_layout & CHANNEL_FRONT_LEFT) {
      if (_matrix_encoding == AV_MATRIX_ENCODING_DOLBY ||
          _matrix_encoding == AV_MATRIX_ENCODING_DPLII) {
        if (unaccounted & (CHANNEL_BACK_LEFT | CHANNEL_SIDE_LEFT)) {
          matrix[FRONT_LEFT][BACK_CENTER] -=
            _surround_mix_level * M_SQRT1_2;
          matrix[FRONT_RIGHT][BACK_CENTER] +=
            _surround_mix_level * M_SQRT1_2;
        } else {
          matrix[FRONT_LEFT][BACK_CENTER] -= _surround_mix_level;
          matrix[FRONT_RIGHT][BACK_CENTER] += _surround_mix_level;
        }
      } else {
        matrix[FRONT_LEFT][BACK_CENTER] +=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][BACK_CENTER] +=
          _surround_mix_level * M_SQRT1_2;
      }
    } else if (out_ch_layout & CHANNEL_FRONT_CENTER) {
      matrix[FRONT_CENTER][BACK_CENTER] +=
        _surround_mix_level * M_SQRT1_2;
    } else {
      assert(false);
    }
  }
  if (unaccounted & CHANNEL_BACK_LEFT) {
    if (out_ch_layout & CHANNEL_BACK_CENTER) {
      matrix[BACK_CENTER][BACK_LEFT] += M_SQRT1_2;
      matrix[BACK_CENTER][BACK_RIGHT] += M_SQRT1_2;
    } else if (out_ch_layout & CHANNEL_SIDE_LEFT) {
      if (in_ch_layout & CHANNEL_SIDE_LEFT) {
        matrix[SIDE_LEFT][BACK_LEFT] += M_SQRT1_2;
        matrix[SIDE_RIGHT][BACK_RIGHT] += M_SQRT1_2;
      } else {
        matrix[SIDE_LEFT][BACK_LEFT] += 1.0;
        matrix[SIDE_RIGHT][BACK_RIGHT] += 1.0;
      }
    } else if (out_ch_layout & CHANNEL_FRONT_LEFT) {
      if (_matrix_encoding == AV_MATRIX_ENCODING_DOLBY) {
        matrix[FRONT_LEFT][BACK_LEFT] -=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_LEFT][BACK_RIGHT] -=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][BACK_LEFT] +=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][BACK_RIGHT] +=
          _surround_mix_level * M_SQRT1_2;
      } else if (_matrix_encoding == AV_MATRIX_ENCODING_DPLII) {
        matrix[FRONT_LEFT][BACK_LEFT] -= _surround_mix_level * SQRT3_2;
        matrix[FRONT_LEFT][BACK_RIGHT] -=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][BACK_LEFT] +=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][BACK_RIGHT] +=
          _surround_mix_level * SQRT3_2;
      } else {
        matrix[FRONT_LEFT][BACK_LEFT] += _surround_mix_level;
        matrix[FRONT_RIGHT][BACK_RIGHT] += _surround_mix_level;
      }
    } else if (out_ch_layout & CHANNEL_FRONT_CENTER) {
      matrix[FRONT_CENTER][BACK_LEFT] +=
        _surround_mix_level * M_SQRT1_2;
      matrix[FRONT_CENTER][BACK_RIGHT] +=
        _surround_mix_level * M_SQRT1_2;
    } else {
      assert(false);
    }
  }

  if (unaccounted & CHANNEL_SIDE_LEFT) {
    if (out_ch_layout & CHANNEL_BACK_LEFT) {
      /* if back channels do not exist in the input, just copy side
         channels to back channels, otherwise mix side into back */
      if (in_ch_layout & CHANNEL_BACK_LEFT) {
        matrix[BACK_LEFT][SIDE_LEFT] += M_SQRT1_2;
        matrix[BACK_RIGHT][SIDE_RIGHT] += M_SQRT1_2;
      } else {
        matrix[BACK_LEFT][SIDE_LEFT] += 1.0;
        matrix[BACK_RIGHT][SIDE_RIGHT] += 1.0;
      }
    } else if (out_ch_layout & CHANNEL_BACK_CENTER) {
      matrix[BACK_CENTER][SIDE_LEFT] += M_SQRT1_2;
      matrix[BACK_CENTER][SIDE_RIGHT] += M_SQRT1_2;
    } else if (out_ch_layout & CHANNEL_FRONT_LEFT) {
      if (_matrix_encoding == AV_MATRIX_ENCODING_DOLBY) {
        matrix[FRONT_LEFT][SIDE_LEFT] -=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_LEFT][SIDE_RIGHT] -=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][SIDE_LEFT] +=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][SIDE_RIGHT] +=
          _surround_mix_level * M_SQRT1_2;
      } else if (_matrix_encoding == AV_MATRIX_ENCODING_DPLII) {
        matrix[FRONT_LEFT][SIDE_LEFT] -= _surround_mix_level * SQRT3_2;
        matrix[FRONT_LEFT][SIDE_RIGHT] -=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][SIDE_LEFT] +=
          _surround_mix_level * M_SQRT1_2;
        matrix[FRONT_RIGHT][SIDE_RIGHT] +=
          _surround_mix_level * SQRT3_2;
      } else {
        matrix[FRONT_LEFT][SIDE_LEFT] += _surround_mix_level;
        matrix[FRONT_RIGHT][SIDE_RIGHT] += _surround_mix_level;
      }
    } else if (out_ch_layout & CHANNEL_FRONT_CENTER) {
      matrix[FRONT_CENTER][SIDE_LEFT] +=
        _surround_mix_level * M_SQRT1_2;
      matrix[FRONT_CENTER][SIDE_RIGHT] +=
        _surround_mix_level * M_SQRT1_2;
    } else {
      assert(false);
    }
  }

  if (unaccounted & CHANNEL_FRONT_LEFT_OF_CENTER) {
    if (out_ch_layout & CHANNEL_FRONT_LEFT) {
      matrix[FRONT_LEFT][FRONT_LEFT_OF_CENTER] += 1.0;
      matrix[FRONT_RIGHT][FRONT_RIGHT_OF_CENTER] += 1.0;
    } else if (out_ch_layout & CHANNEL_FRONT_CENTER) {
      matrix[FRONT_CENTER][FRONT_LEFT_OF_CENTER] += M_SQRT1_2;
      matrix[FRONT_CENTER][FRONT_RIGHT_OF_CENTER] += M_SQRT1_2;
    } else {
      assert(false);
    }
  }
  /* mix LFE into front left/right or center */
  if (unaccounted & CHANNEL_LOW_FREQUENCY) {
    if (out_ch_layout & CHANNEL_FRONT_CENTER) {
      matrix[FRONT_CENTER][LOW_FREQUENCY] += _lfe_mix_level;
    } else if (out_ch_layout & CHANNEL_FRONT_LEFT) {
      matrix[FRONT_LEFT][LOW_FREQUENCY] += _lfe_mix_level * M_SQRT1_2;
      matrix[FRONT_RIGHT][LOW_FREQUENCY] += _lfe_mix_level * M_SQRT1_2;
    } else {
      assert(false);
    }
  }

  for (uint32_t out_i = 0, i = 0; i < 32; i++) {
    double sum = 0;
    int in_i = 0;
    if ((out_ch_layout & (1U << i)) == 0) {
      continue;
    }
    for (uint32_t j = 0; j < 32; j++) {
      if ((in_ch_layout & (1U << j)) == 0) {
        continue;
      }
      if (i < FF_ARRAY_ELEMS(matrix) && j < FF_ARRAY_ELEMS(matrix[0])) {
        _matrix[out_i][in_i] = matrix[i][j];
      } else {
        _matrix[out_i][in_i] =
          i == j && (in_ch_layout & out_ch_layout & (1U << i));
      }
      sum += fabs(_matrix[out_i][in_i]);
      in_i++;
    }
    maxcoef = std::max(maxcoef, sum);
    out_i++;
  }

  if (_rematrix_volume < 0) {
    maxcoef = -_rematrix_volume;
  }

  if (_format == CUBEB_SAMPLE_S16NE) {
    maxval = 1.0;
  } else {
    maxval = INT_MAX;
  }

  if (maxcoef > maxval || _rematrix_volume < 0) {
    maxcoef /= maxval;
    for (uint32_t i = 0; i < CHANNELS_MAX; i++)
      for (uint32_t j = 0; j < CHANNELS_MAX; j++) {
        _matrix[i][j] /= maxcoef;
      }
  }

  if (_rematrix_volume > 0 && _rematrix_volume != 1) {
    for (uint32_t i = 0; i < CHANNELS_MAX; i++)
      for (uint32_t j = 0; j < CHANNELS_MAX; j++) {
        _matrix[i][j] *= _rematrix_volume;
      }
  }

  if (_format == CUBEB_SAMPLE_FLOAT32NE) {
    for (uint32_t i = 0; i < FF_ARRAY_ELEMS(_matrix[0]); i++) {
      for (uint32_t j = 0; j < FF_ARRAY_ELEMS(_matrix[0]); j++) {
        _matrix_flt[i][j] = _matrix[i][j];
      }
    }
  }

  return 0;
}

int MixerContext::init()
{
  int r = auto_matrix();
  if (r) {
    return r;
  }

  // Determine if matrix operation would overflow
  if (_format == CUBEB_SAMPLE_S16NE) {
    int maxsum = 0;
    for (uint32_t i = 0; i < _out_ch_count; i++) {
      double rem = 0;
      int sum = 0;

      for (uint32_t j = 0; j < _in_ch_count; j++) {
        double target = _matrix[i][j] * 32768 + rem;
        int value = lrintf(target);
        rem += target - value;
        sum += std::abs(value);
      }
      maxsum = std::max(maxsum, sum);
    }
    if (maxsum > 32768) {
      _clipping = true;
    }
  }

  // FIXME quantize for integers
  for (uint32_t i = 0; i < CHANNELS_MAX; i++) {
    int ch_in = 0;
    for (uint32_t j = 0; j < CHANNELS_MAX; j++) {
      _matrix32[i][j] = lrintf(_matrix[i][j] * 32768);
      if (_matrix[i][j]) {
        _matrix_ch[i][++ch_in] = j;
      }
    }
    _matrix_ch[i][0] = ch_in;
  }

  return 0;
}

static int rematrix(const MixerContext * s, void * out, void * in, uint32_t frames)
{
  for (uint32_t out_i = 0; out_i < s->_out_ch_count; out_i++) {
    switch (s->_matrix_ch[out_i][0]) {
      case 0:
        if (s->_format == CUBEB_SAMPLE_FLOAT32NE) {
          float* outf = static_cast<float*>(out) + out_i;
          for (uint32_t i = 0; i < frames; i++) {
            outf[i * s->_out_ch_count] = 0;
          }
        } else {
          assert(s->_format == CUBEB_SAMPLE_S16NE);
          int16_t* outi = static_cast<int16_t*>(out) + out_i;
          for (uint32_t i = 0; i < frames; i++) {
            outi[i * s->_out_ch_count] = 0;
          }
        }
        break;
      case 1: {
        int in_i = s->_matrix_ch[out_i][1];
        if (s->_format == CUBEB_SAMPLE_FLOAT32NE) {
          float* outf = static_cast<float*>(out) + out_i;
          float* inf = static_cast<float*>(in) + in_i;
          copy(outf,
               s->_out_ch_count,
               inf,
               s->_in_ch_count,
               s->_matrix_flt[out_i][in_i],
               frames);
        } else {
          assert(s->_format == CUBEB_SAMPLE_S16NE);
          int16_t* outi = static_cast<int16_t*>(out) + out_i;
          int16_t* ini = static_cast<int16_t*>(in) + in_i;
          if (s->_clipping) {
            copy<int16_t, int, true>(outi,
                                     s->_out_ch_count,
                                     ini,
                                     s->_in_ch_count,
                                     s->_matrix32[out_i][in_i],
                                     frames);
          } else {
            copy<int16_t, int, false>(outi,
                                      s->_out_ch_count,
                                      ini,
                                      s->_in_ch_count,
                                      s->_matrix32[out_i][in_i],
                                      frames);
          }
        }
      } break;
      case 2:
        if (s->_format == CUBEB_SAMPLE_FLOAT32NE) {
          float* outf = static_cast<float*>(out) + out_i;
          float* inf1 = static_cast<float*>(in) + s->_matrix_ch[out_i][1];
          float* inf2 = static_cast<float*>(in) + s->_matrix_ch[out_i][2];
          sum2(outf,
               s->_out_ch_count,
               inf1,
               inf2,
               s->_in_ch_count,
               s->_matrix_flt[out_i][s->_matrix_ch[out_i][1]],
               s->_matrix_flt[out_i][s->_matrix_ch[out_i][2]],
               frames);
        } else {
          assert(s->_format == CUBEB_SAMPLE_S16NE);
          int16_t* outi = static_cast<int16_t*>(out) + out_i;
          int16_t* ini1 = static_cast<int16_t*>(in) + s->_matrix_ch[out_i][1];
          int16_t* ini2 = static_cast<int16_t*>(in) + s->_matrix_ch[out_i][2];
          if (s->_clipping) {
            sum2<int16_t, int, true>(
              outi,
              s->_out_ch_count,
              ini1,
              ini2,
              s->_in_ch_count,
              s->_matrix32[out_i][s->_matrix_ch[out_i][1]],
              s->_matrix32[out_i][s->_matrix_ch[out_i][2]],
              frames);
          } else {
            sum2<int16_t, int, true>(
              outi,
              s->_out_ch_count,
              ini1,
              ini2,
              s->_in_ch_count,
              s->_matrix32[out_i][s->_matrix_ch[out_i][1]],
              s->_matrix32[out_i][s->_matrix_ch[out_i][2]],
              frames);
          }
        }
        break;
      default:
        if (s->_format == CUBEB_SAMPLE_FLOAT32NE) {
          float* outf = static_cast<float*>(out) + out_i;
          float* inf = static_cast<float*>(in);
          for (uint32_t i = 0; i < frames; i++) {
            float v = 0;
            for (uint32_t j = 0; j < s->_matrix_ch[out_i][0]; j++) {
              uint32_t in_i = s->_matrix_ch[out_i][1 + j];
              v += *(inf + in_i + i * s->_in_ch_count) *
                   s->_matrix_flt[out_i][in_i];
            }
            outf[i * s->_out_ch_count] = v;
          }
        } else {
          assert(s->_format == CUBEB_SAMPLE_S16NE);
          int16_t* outi = static_cast<int16_t*>(out) + out_i;
          int16_t* ini = static_cast<int16_t*>(in);
          for (uint32_t i = 0; i < frames; i++) {
            int v = 0;
            for (uint32_t j = 0; j < s->_matrix_ch[out_i][0]; j++) {
              uint32_t in_i = s->_matrix_ch[out_i][1 + j];
              v +=
                *(ini + in_i + i * s->_in_ch_count) * s->_matrix32[out_i][in_i];
            }
            if (s->_clipping) {
              outi[i * s->_out_ch_count] = R_clip(v);
            } else {
              outi[i * s->_out_ch_count] = R(v);
            }
          }
        }
    }
  }
  return 0;
}

struct cubeb_mixer
{
  cubeb_mixer(cubeb_sample_format format,
              cubeb_channel_layout in_layout,
              cubeb_channel_layout out_layout)
    : _context(format, in_layout, out_layout)
  {
  }

  int mix(unsigned long frames,
          void * input_buffer,
          unsigned long input_buffer_size,
          void * output_buffer,
          unsigned long output_buffer_size) const
  {
    if (!valid()) {
      return -1;
    }
    if (frames <= 0) {
      return 0;
    }

    // Check if output buffer is of sufficient size.
    size_t size_read_needed =
      frames * _context._in_ch_count * cubeb_sample_size(_context._format);
    if (input_buffer_size < size_read_needed) {
      // We don't have enough data to read!
      return -1;
    }
    if (output_buffer_size * _context._in_ch_count <
        size_read_needed * _context._out_ch_count) {
      return -1;
    }
    return rematrix(&_context, output_buffer, input_buffer, frames);
  }

  bool valid() const { return _context._valid; }

  virtual ~cubeb_mixer(){};

  MixerContext _context;
};

cubeb_mixer* cubeb_mixer_create(cubeb_sample_format format,
                                cubeb_channel_layout in_layout,
                                cubeb_channel_layout out_layout)
{
  if (!MixerContext::sane_layout(in_layout) ||
      !MixerContext::sane_layout(out_layout)) {
    return nullptr;
  }
  return new cubeb_mixer(format, in_layout, out_layout);
}

void cubeb_mixer_destroy(cubeb_mixer * mixer)
{
  delete mixer;
}

int cubeb_mixer_mix(cubeb_mixer * mixer,
                    unsigned long frames,
                    void* input_buffer,
                    unsigned long input_buffer_size,
                    void* output_buffer,
                    unsigned long output_buffer_size)
{
  return mixer->mix(
    frames, input_buffer, input_buffer_size, output_buffer, output_buffer_size);
}
