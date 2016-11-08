#include "cubeb_ringbuffer.h"
#include <iostream>
#include <thread>
#include <unistd.h>
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
            assert(false);
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
void test_ring(queue<T>& buf, int channels, int capacity_frames)
{
  std::unique_ptr<T[]> seq(new T[capacity_frames * channels]);
  sequence_generator<T> gen(channels);
  sequence_verifier<T> checker(channels);

  int iterations = 1002;

  const int block_size = 128;

  while(iterations--) {
    gen.get(seq.get(), block_size);
    int rv = buf.enqueue(seq.get(), block_size);
    assert(rv == block_size);
    PodZero(seq.get(), block_size);
    rv = buf.dequeue(seq.get(), block_size);
    assert(rv == block_size);
    checker.check(seq.get(), block_size);
  }
}

template<typename T>
void test_ring_multi(lock_free_queue<T>& buf, int channels, int capacity_frames)
{
  sequence_verifier<T> checker(channels);
  std::unique_ptr<T[]> out_buffer(new T[capacity_frames * channels]);

  const int block_size = 128;

  std::thread t([&buf, capacity_frames, channels] {
    int iterations = 1002;
    std::unique_ptr<T[]> in_buffer(new T[capacity_frames * channels]);
    sequence_generator<T> gen(channels);

    while(iterations--) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
      gen.get(in_buffer.get(), block_size);
      int rv = buf.enqueue(in_buffer.get(), block_size);
      assert(rv <= block_size);
      if (rv != block_size) {
        gen.rewind(block_size - rv);
      }
    }
  });

  uint32_t remaining = 1002;

  while(remaining--) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    int rv = buf.dequeue(out_buffer.get(), block_size);
    assert(rv <= block_size);
    checker.check(out_buffer.get(), rv);
  }

  t.join();
}

void basic_api_test()
{
  queue<float> ring(2, 128);

  assert(ring.capacity() == 128);

  assert(ring.empty());
  assert(!ring.full());

  int rv = ring.enqueue_silence(63);

  assert(rv == 63);
  assert(ring.available_read() == 63);
  assert(ring.available_write() == 65);
  assert(!ring.empty());
  assert(!ring.full());

  rv = ring.enqueue_silence(65);

  assert(rv = 65);
  assert(!ring.empty());
  assert(ring.full());
  assert(ring.available_read() == 128);
  assert(ring.available_write() == 0);

  rv = ring.dequeue(nullptr, 63);

  assert(!ring.empty());
  assert(!ring.full());
  assert(ring.available_read() == 65);
  assert(ring.available_write() == 63);

  rv = ring.dequeue(nullptr, 65);

  assert(ring.empty());
  assert(!ring.full());
  assert(ring.available_read() == 0);
  assert(ring.available_write() == 128);
}

int main()
{
  /* Basic API test. */
  basic_api_test();

  /* Single thread testing. */
  /* Test mono to 9.1 */
  for (size_t channels = 1; channels < 10; channels++) {
    /* Use non power-of-two numbers to catch edge-cases. */
    for (size_t capacity_frames = 199; capacity_frames < 1277; capacity_frames+=37) {
      queue<float> ring(channels, capacity_frames);
      test_ring(ring, channels, capacity_frames);
    }
  }

  /* Multi thread testing */
  for (size_t channels = 1; channels < 10; channels++) {
    /* Use non power-of-two numbers to catch edge-cases. */
    for (size_t capacity_frames = 199; capacity_frames < 1277; capacity_frames+=37) {
      lock_free_queue<long> ring(channels, capacity_frames);
      test_ring_multi(ring, channels, capacity_frames);
    }
  }

  return 0;
}
