/*
 * Copyright © 2022 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

/* cubeb_triple_buffer test  */
#include "gtest/gtest.h"
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600
#endif
#include "cubeb/cubeb.h"
#include "cubeb_triple_buffer.h"
#include <atomic>
#include <math.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

#include "common.h"

TEST(cubeb, triple_buffer)
{
  struct AB {
    uint64_t a;
    uint64_t b;
  };
  triple_buffer<AB> buffer;

  std::atomic<bool> finished = {false};

  ASSERT_TRUE(!buffer.updated());

  auto t = std::thread([&finished, &buffer] {
    AB ab;
    ab.a = 0;
    ab.b = UINT64_MAX;
    uint64_t counter = 0;
    do {
      buffer.write(ab);
      ab.a++;
      ab.b--;
    } while (counter++ < 1e6 && ab.a <= UINT64_MAX && ab.b != 0);
    finished.store(true);
  });

  AB ab;
  AB old_ab;
  old_ab.a = 0;
  old_ab.b = UINT64_MAX;

  // Wait to have at least one value produced.
  while (!buffer.updated()) {
  }

  // Check that the values are increasing (resp. descreasing) monotonically.
  while (!finished) {
    ab = buffer.read();
    ASSERT_GE(ab.a, old_ab.a);
    ASSERT_LE(ab.b, old_ab.b);
    old_ab = ab;
  }

  t.join();

  buffer.invalidate();
  ASSERT_FALSE(buffer.updated());
}

// cubeb_aaudio allocates its context, and the streams embedded in it, with a
// default-initializing `new cubeb;`, so a triple_buffer can be read before the
// producer has published anything. Check that this yields zeroes rather than
// whatever previously occupied that memory.
TEST(cubeb, triple_buffer_read_before_write)
{
  struct AB {
    uint64_t a;
    uint64_t b;
  };

  alignas(triple_buffer<AB>) uint8_t mem[sizeof(triple_buffer<AB>)];
  memset(mem, 0xab, sizeof(mem));
  // Default-initialization, as in cubeb_aaudio: value-initialization here
  // would zero the whole object and defeat the point of the test.
  auto * buffer = new (mem) triple_buffer<AB>;

  AB ab = buffer->read();
  ASSERT_EQ(ab.a, 0u);
  ASSERT_EQ(ab.b, 0u);

  // Same after invalidate(), which rotates the indices without publishing.
  buffer->invalidate();
  ab = buffer->read();
  ASSERT_EQ(ab.a, 0u);
  ASSERT_EQ(ab.b, 0u);

  buffer->~triple_buffer();
}
