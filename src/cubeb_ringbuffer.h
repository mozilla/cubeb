/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#include <memory>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include "cubeb_utils.h"


/* This enum allow choosing the behaviour of the queue. */
enum ThreadSafety
{
  /* No attempt to synchronize the queue is made. The queue is only safe when
   * used on a single thread. */
  Unsafe,
  /** Atomics are used to synchronize read and write. The queue is safe to used
   * from two thread: one producer, one consumer. */
  Safe
};

/** Policy to enable thread safety on the queue. */
template<ThreadSafety>
struct ThreadSafePolicy;

/** Policy for thread-safe internal index for the queue.
 *
 * For now, we use 32-bits index. 64-bits index could be used if needed, but it
 * does not seem useful for real-time audio: that would mean we're buffering
 * quite a lot of data if we go over the 32-bits limit.
 */
template<>
struct ThreadSafePolicy<Safe>
{
  typedef std::atomic<int> IndexType;
};

/**
 * This is the version with a simple `int` for index, for use when only a single
 * thread is producing and releasing data.
 */
template<>
struct ThreadSafePolicy<Unsafe>
{
  typedef int IndexType;
};

/**
 * Single producer single consumer lock-free and wait-free ring buffer.
 *
 * This data structure allow producing data from one thread, and consuming it on
 * another thread, safely and without explicit synchronization. If used on two
 * threads, this data structure uses atomics for thread safety. It is possible
 * to disable the use of atomics at compile time and only use this data
 * structure on one thread.
 *
 * The role for the producer and the consumer must be constant, i.e., the same
 * thread should always be the producer and another thread should always be the
 * consumer.
 *
 * The public interface of this class uses frames.
 *
 * Some words about the inner workings of this class:
 * - Capacity is fixed. Only one allocation is performed, in the constructor.
 *   When reading and writing, the return value of the method allow checking is
 *   the ring buffer is empty or full.
 * - We always keep the read index at least one element ahead of the write
 *   index, so we can distinguish between an empty and a full ring buffer: an
 *   empty ring buffer is when the write index is at the same position as the
 *   write index. A full buffer is when the write index is exactly one position
 *   before the read index.
 * - We synchronize updates to the read index after having read the data, and
 *   the write index after having written the data. This means that the each
 *   thread can only touch a portion of the buffer that is not touched by the
 *   other thread.
 * - Caller is expected to provide an output buffer. When writing to the queue,
 *   elements are copied. When reading from the queue, the user is expected to
 *   provide a buffer. Because this is a ring buffer, data might not be
 *   contiguous in memory, providing an external buffer to copy into is an easy
 *   way to have linear data for further processing.
 */
template <typename T,
          ThreadSafety Safety = ThreadSafety::Safe>
class ring_buffer_base
{
public:
  /**
   * Constructor for a ring buffer.
   *
   * This performs an allocation, but is the only allocation that will happen
   * for the life time of a `ring_buffer_base`.
   *
   * @param channel_count the number of channels of the stream for this ring buffer.
   * @param capacity_in_frames The maximum number of frames this ring buffer will hold.
   */
  ring_buffer_base(int channel_count, int capacity_in_frames)
    : read_index_(0)
    , write_index_(0)
    , channel_count_(channel_count)
    /* One element more to distinguish from emtpy and full buffer. */
    , capacity_(frames_to_samples(capacity_in_frames + 1))
  {
    static_assert(std::is_trivial<T>::value,
                  "ring_buffer_base requires trivial type");

    assert(storage_capacity() < std::numeric_limits<T>::max() &&
           "buffer to large for the type of index used.");
    assert(channel_count_ > 0);
    assert(capacity_in_frames > 0);

    data_.reset(new T[frames_to_samples(storage_capacity())]);
    PodZero(data_.get(), storage_capacity());
  }
  /**
   * Push `count` silent frames into the ring buffer.
   *
   * Only safely called on the producer thread.
   *
   * @param count The number of frames of silence to enqueue.
   * @return The number of frames of silence actually enqueued.
   */
  int enqueue_silence(int count)
  {
    return enqueue(nullptr, count);
  }
  /**
   * Push `count` frames of audio in the ring buffer.
   *
   * Only safely called on the producer thread.
   *
   * @param elements a pointer to a buffer containing at least `count` audio
   * frames. If `elements` is `nullptr`, silence is enqueued.
   * @param count The number of audio frames to read from `elements`
   * @return The number of frames successfully copy from elements and inserted
   * into the ring buffer.
   */
  int enqueue(T * elements, int count)
  {
    int rd_idx = read_index_;
    int wr_idx = write_index_;

    if (full_internal(rd_idx, wr_idx)) {
      return 0;
    }

    int to_write = std::min(available_write_internal(rd_idx, wr_idx),
                            frames_to_samples(count));

    /* First part, from the write index to the end of the array. */
    int first_part = std::min(storage_capacity() - wr_idx,
                              to_write);
    /* Second part, from the beginning of the array */
    int second_part = std::max(to_write - first_part, 0);

    if (elements) {
      PodCopy(data_.get() + wr_idx, elements, first_part);
      PodCopy(data_.get(), elements + first_part, second_part);
    } else {
      PodZero(data_.get() + wr_idx, first_part);
      PodZero(data_.get(), second_part);
    }

    increment_index(wr_idx, to_write);

    write_index_ = wr_idx;

    return samples_to_frames(to_write);
  }
  /**
   * Retrieve at most `count` frames from the ring buffer, and copy them to
   * `elements`, if non-null.
   *
   * Only safely called on the consumer side.
   *
   * @param elements A pointer to a buffer with space for at least `count`
   * frames of audio. If `elements` is `nullptr`, elements will be discarded.
   * @param count The maximum number of elements to dequeue.
   * @return The number of frames of audio written to `elements`.
   */
  int dequeue(T * elements, int count)
  {
    int wr_idx = write_index_;
    int rd_idx = read_index_;

    if (empty_internal(rd_idx, wr_idx)) {
      return 0;
    }

    int to_read = std::min(available_read_internal(rd_idx, wr_idx),
                           frames_to_samples(count));

    int first_part = std::min(storage_capacity() - rd_idx, to_read);
    int second_part = std::max(to_read - first_part, 0);

    if (elements) {
      PodCopy(elements, data_.get() + rd_idx, first_part); 
      PodCopy(elements + first_part, data_.get(), second_part);
    }

    increment_index(rd_idx, to_read);

    read_index_ = rd_idx;

    return samples_to_frames(to_read);
  }
  /**
   * Get the number of available frames of audio for consuming.
   *
   * Only safely called on the consumer thread.
   *
   * @return The number of available frames of audio for reading.
   */
  int available_read() const
  {
    return samples_to_frames(available_read_internal(read_index_,
                                                     write_index_));
  }
  /**
   * Get the number of available frames of audio for consuming.
   *
   * Only safely called on the producer thread.
   *
   * @return The number of empty slots in the buffer, available for writing.
   */
  int available_write() const
  {
    return samples_to_frames(available_write_internal(read_index_,
                                                      write_index_));
  }
  /**
   * Get total capacity, in frames, for this ring buffer.
   *
   * Can be called safely on any thread.
   *
   * @return The maximum capacity, in frames, of this ring buffer.
   */
  int capacity() const
  {
    return samples_to_frames(capacity_) - 1;
  }
  /** Return true if the ring buffer is empty.
   *
   * Can be called safely on any thread.
   *
   * @return true if the ring buffer is empty, false otherwise.
   **/
  bool empty() const
  {
    return empty_internal(read_index_, write_index_);
  }
  /** Return true if the ring buffer is full.
   *
   * Can be called safely on any thread.
   *
   * This happens if the write index is exactly one element behind the read
   * index.
   *
   * @return true if the ring buffer is full, false otherwise.
   **/
  bool full() const
  {
    return full_internal(read_index_, write_index_);
  }
private:
  /** Return true if the ring buffer is empty.
   *
   * @param read_index the read index to consider
   * @param write_index the write index to consider
   * @return true if the ring buffer is empty, false otherwise.
   **/
  bool empty_internal(int read_index, int write_index) const
  {
    return write_index == read_index;
  }
  /** Return true if the ring buffer is full.
   *
   * This happens if the write index is exactly one element behind the read
   * index.
   *
   * @param read_index the read index to consider
   * @param write_index the write index to consider
   * @return true if the ring buffer is full, false otherwise.
   **/
  bool full_internal(int read_index, int write_index) const
  {
    return (write_index + channel_count_) % capacity_ == read_index;
  }
  /**
   * Return the size of the storage. It is one more than the number of elements
   * that can be stored in the buffer.
   *
   * @return the number of elements that can be stored in the buffer.
   */
  int storage_capacity() const
  {
    return capacity_;
  }
  /**
   * Convert from frames to samples.
   *
   * @param frames A number of frames
   * @return int The number of samples.
   */
  int frames_to_samples(int frames) const
  {
     return frames * channel_count_;
  }
  /**
   * Convert from samples to frames.
   *
   * @param frames A number of samples
   * @return int The number of frames.
   */
  int samples_to_frames(int samples) const
  {
     return samples / channel_count_;
  }
  /**
   * Returns the number of samples available for reading.
   *
   * @return the number of available samples for reading.
   */
  int available_read_internal(int read_index, int write_index) const
  {
    if (write_index >= read_index) {
      return write_index - read_index;
    } else {
      return write_index + capacity_ - read_index;
    }
  }
  /**
   * Returns the number of empty elements, available for writing.
   *
   * @return the number of elements that can be written into the array.
   */
  int available_write_internal(int read_index, int write_index) const
  {
    /* We substract one frame (`channel_count_` samples) here to always keep at
     * least one element free in the buffer, to distinguish between full and
     * empty array. */
    int rv = read_index - write_index - channel_count_;
    if (write_index >= read_index) {
      rv += capacity_;
    }
    return rv;
  }
  /**
   * Increments an index, wrapping it around the storage.
   *
   * @param index a reference to the index to increment.
   * @param increment the number by which `index` is incremented.
   */
  template <typename IndexType>
  void increment_index(IndexType& index, uint32_t increment) const
  {
    /** Don't make this two operations, `index` might be atomic, we want other
     * threads to see either the old or the new value, but not an intermediary
     * computation step: it should only be assigned once. */
    index = (index + increment) % capacity_;
  }
  /** Index at which the oldest frame is at. In samples. */
  typename ThreadSafePolicy<Safety>::IndexType read_index_;
  /** Index at which to write new frames. In samples. `write_index` is always at
   * most one element behind `read_index_`. */
  typename ThreadSafePolicy<Safety>::IndexType write_index_;
  /** Channel count for this ring buffer. */
  const int channel_count_;
  /** Number of samples at maximum that can be stored in the ring buffer. */
  const int capacity_;
  /** Data storage */
  std::unique_ptr<T[]> data_;
};

/**
 * Lock-free instantiation of the `ring_buffer_base` type. This is safe to use
 * from two threads, one producer, one consumer (that never change role),
 * without explicit synchronization.
 */
template<typename T>
using lock_free_queue = ring_buffer_base<T, Safe>;
/**
 * An instantiation of the `ring_buffer_base` type, to be used on a single
 * thread: it is not safe to use from multiple threads without explicit external
 * synchronization.
 */
template<typename T>
using queue = ring_buffer_base<T, Unsafe>;
