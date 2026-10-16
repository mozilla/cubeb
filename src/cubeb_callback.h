/*
 * Copyright © 2026 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#ifndef CUBEB_CALLBACK_H
#define CUBEB_CALLBACK_H

#include "cubeb/cubeb.h"
#include <atomic>

// A short application return can be hidden by buffered resampler output. Keep
// this state on the stream so replacing the resampler cannot resume callbacks.
class cubeb_callback_state {
public:
  long call(cubeb_data_callback callback, cubeb_stream * stream,
            void * user_ptr, void const * input, void * output, long frames)
  {
    long result = terminal_result_.load();
    if (result <= 0) {
      return result;
    }
    long rv = callback(stream, user_ptr, input, output, frames);
    if (rv < frames) {
      terminal_result_.store(rv < 0 ? rv : 0);
    }
    return rv;
  }

  bool draining() const { return terminal_result_.load() == 0; }

  bool failed() const { return terminal_result_.load() < 0; }

  // Only reset after callbacks have stopped, on an explicit application start.
  // The resampler must also be reset if it has entered drain mode.
  void reset() { terminal_result_.store(1); }

private:
  // Positive means callbacks are enabled; zero means drain, negative error.
  std::atomic<long> terminal_result_{1};
};

#endif // CUBEB_CALLBACK_H
