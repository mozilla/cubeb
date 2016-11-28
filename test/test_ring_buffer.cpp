/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#define NOMINMAX

#include "gtest/gtest.h"
#include "cubeb_ringbuffer.h"
#include <iostream>
#include <thread>
#include <chrono>

/* Generate a monotonically increasing sequence of numbers. */
template<typename T>
class sequence_generator
{
public:
  sequence_generator(size_t channels)
    : channels(channels)
  { }
  void get(T * elements, size_t frames)
  {
    for (size_t i = 0; i < frames; i++) {
      for (size_t c = 0; c < channels; c++) {
        elements[i * channels + c] = static_cast<T>(index_);
      }
      index_++;
    }
  }
  void rewind(size_t frames)
  {
    index_ -= frames;
  }
private:
  size_t index_ = 0;
  size_t channels = 0;
};

/* Checks that a sequence is monotonically increasing. */
template<typename T>
class sequence_verifier
{
  public:
  sequence_verifier(size_t channels)
    : channels(channels)
  { }
    void check(T * elements, size_t frames)
    {
      for (size_t i = 0; i < frames; i++) {
        for (size_t c = 0; c < channels; c++) {
          if (elements[i * channels + c] != static_cast<T>(index_)) {
            std::cerr << "Element " << i << " is different. Expected " 
              << static_cast<T>(index_) << ", got " << elements[i]
              << ". (channel count: " << channels << ")." << std::endl;
            ASSERT_TRUE(false);
          }
        }
        index_++;
      }
    }
  private:
    size_t index_ = 0;
    size_t channels = 0;
};

template<typename T>
void test_ring(audio_ring_buffer<T>& buf, int channels, int capacity_frames)
{
  std::unique_ptr<T[]> seq(new T[capacity_frames * channels]);
  sequence_generator<T> gen(channels);
  sequence_verifier<T> checker(channels);

  int iterations = 1002;

  const int block_size = 128;

  while(iterations--) {
    gen.get(seq.get(), block_size);
    int rv = buf.enqueue(seq.get(), block_size);
    ASSERT_EQ(rv, block_size);
    PodZero(seq.get(), block_size);
    rv = buf.dequeue(seq.get(), block_size);
    ASSERT_EQ(rv, block_size);
    checker.check(seq.get(), block_size);
  }
}

template<typename T>
void test_ring_multi(lock_free_audio_ring_buffer<T>& buf, int channels, int capacity_frames)
{
  sequence_verifier<T> checker(channels);
  std::unique_ptr<T[]> out_buffer(new T[capacity_frames * channels]);

  const int block_size = 128;

  std::thread t([&buf, capacity_frames, channels, block_size] {
    int iterations = 1002;
    std::unique_ptr<T[]> in_buffer(new T[capacity_frames * channels]);
    sequence_generator<T> gen(channels);

    while(iterations--) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      gen.get(in_buffer.get(), block_size);
      int rv = buf.enqueue(in_buffer.get(), block_size);
      ASSERT_TRUE(rv <= block_size);
      if (rv != block_size) {
        gen.rewind(block_size - rv);
      }
    }
  });

  int remaining = 1002;

  while(remaining--) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    int rv = buf.dequeue(out_buffer.get(), block_size);
    ASSERT_TRUE(rv <= block_size);
    checker.check(out_buffer.get(), rv);
  }

  t.join();
}

template<typename T>
void basic_api_test(T& ring)
{
  ASSERT_EQ(ring.capacity(), 128);

  ASSERT_TRUE(ring.empty());
  ASSERT_TRUE(!ring.full());

  int rv = ring.enqueue_default(63);

  ASSERT_TRUE(rv == 63);
  ASSERT_EQ(ring.available_read(), 63);
  ASSERT_EQ(ring.available_write(), 65);
  ASSERT_TRUE(!ring.empty());
  ASSERT_TRUE(!ring.full());

  rv = ring.enqueue_default(65);

  ASSERT_EQ(rv, 65);
  ASSERT_TRUE(!ring.empty());
  ASSERT_TRUE(ring.full());
  ASSERT_EQ(ring.available_read(), 128);
  ASSERT_EQ(ring.available_write(), 0);

  rv = ring.dequeue(nullptr, 63);

  ASSERT_TRUE(!ring.empty());
  ASSERT_TRUE(!ring.full());
  ASSERT_EQ(ring.available_read(), 65);
  ASSERT_EQ(ring.available_write(), 63);

  rv = ring.dequeue(nullptr, 65);

  ASSERT_TRUE(ring.empty());
  ASSERT_TRUE(!ring.full());
  ASSERT_EQ(ring.available_read(), 0);
  ASSERT_EQ(ring.available_write(), 128);
}

TEST(cubeb, ring_buffer)
{
  /* Basic API test. */
  const int min_channels = 1;
  const int max_channels = 10;
  const int min_capacity = 199;
  const int max_capacity = 1277;
  const int capacity_increment = 27;

  queue<float> q1(128);
  basic_api_test(q1);
  queue<short> q2(128);
  basic_api_test(q2);
  lock_free_queue<float> q3(128);
  basic_api_test(q3);
  lock_free_queue<short> q4(128);
  basic_api_test(q4);

  for (size_t channels = min_channels; channels < max_channels; channels++) {
    audio_ring_buffer<float> q5(channels, 128);
    basic_api_test(q5);
    audio_ring_buffer<short> q6(channels, 128);
    basic_api_test(q6);
    lock_free_audio_ring_buffer<float> q7(channels, 128);
    basic_api_test(q7);
    lock_free_audio_ring_buffer<short> q8(channels, 128);
    basic_api_test(q8);
  }

  /* Single thread testing. */
  /* Test mono to 9.1 */
  for (size_t channels = min_channels; channels < max_channels; channels++) {
    /* Use non power-of-two numbers to catch edge-cases. */
    for (size_t capacity_frames = min_capacity;
         capacity_frames < max_capacity; capacity_frames+=capacity_increment) {
      audio_ring_buffer<float> ring(channels, capacity_frames);
      test_ring(ring, channels, capacity_frames);
    }
  }

  /* Multi thread testing */
  for (size_t channels = max_channels; channels < min_channels; channels++) {
    /* Use non power-of-two numbers to catch edge-cases. */
    for (size_t capacity_frames = min_capacity;
         capacity_frames < max_capacity; capacity_frames+=capacity_increment) {
      lock_free_audio_ring_buffer<short> ring(channels, capacity_frames);
      test_ring_multi(ring, channels, capacity_frames);
    }
  }
}
