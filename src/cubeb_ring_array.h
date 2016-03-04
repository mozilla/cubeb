/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef CUBEB_RING_ARRAY_H
#define CUBEB_RING_ARRAY_H

#if defined(__cplusplus)
extern "C" {
#endif

/** Ring array of pointers is used to hold buffers. In case that
    asynchronus producer/consumer callbacks do not arrive in a
    repeated order the ring array stores the buffers and fetch
    them in the correct order. */
#define RING_ARRAY_CAPACITY 8

typedef struct {
  void* pointer_array[RING_ARRAY_CAPACITY];   /**< Array that hold pointers of the allocated space for the buffers. */
  unsigned int tail;                          /**< Index of the last element (first to deliver). */
  int count;                                  /**< Number of elements in the array. */
  unsigned int capacity;                      /**< Total length of the array. */
  pthread_mutex_t mutex;                      /**< Mutex to synchronize store/fetch. */
} ring_array;

/** Initialize the ring array.
    @param ra The ring_array pointer of allocated structure.
    @retval 0 on success. */
int
ring_array_init(ring_array * ra)
{
  ra->capacity = RING_ARRAY_CAPACITY;
  ra->tail = 0;
  ra->count = 0;
  memset(ra->pointer_array, 0, sizeof(ra->pointer_array));

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
  int ret = pthread_mutex_init(&ra->mutex, &attr);
  assert(0 == ret);
  pthread_mutexattr_destroy(&attr);

  assert(ra->pointer_array[0] == NULL);

  return ret;
}

/** Set the allocated space to store the data.
    This must be done before store/fetch.
    @param ra The ring_array pointer.
    @param data Pointer to allocated space of buffers.
    @param index Index between 0 and capacity-1 to store the allocated data buffer.
    @retval The data pointer on success or NULL on failure. */
void *
ring_array_set_data(ring_array * ra, void * data, unsigned int index)
{
  if (index < ra->capacity) {
    ra->pointer_array[index] = data;
    return data;
  }
  return NULL;
}

/** Destroy the ring array.
    @param ra The ring_array pointer.*/
void
ring_array_destroy(ring_array * ra)
{
  pthread_mutex_destroy(&ra->mutex);
}

/** Get the allocated buffer to be stored with fresh data.
    @param ra The ring_array pointer.
    @retval Pointer of the allocated space to be stored with fresh data or NULL if full. */
void *
ring_array_store_buffer(ring_array * ra)
{
  int rv = pthread_mutex_lock(&ra->mutex);
  assert(rv == 0);

  assert(ra->pointer_array[0] != NULL);

  if (ra->count == (int)ra->capacity) {
    pthread_mutex_unlock(&ra->mutex);
    return NULL;
  }

  assert(ra->count == 0 || (ra->tail + ra->count) % ra->capacity != ra->tail);
  void * ret = ra->pointer_array[(ra->tail + ra->count) % ra->capacity];

  ++ra->count;
  assert(ra->count <= (int)ra->capacity);

  pthread_mutex_unlock(&ra->mutex);
  return ret;
}

/** Get the next available buffer with data.
    @param ra The ring_array pointer.
    @retval Pointer of the next in order data buffer or NULL if empty. */
void *
ring_array_fetch_buffer(ring_array * ra)
{
  int rv = pthread_mutex_lock(&ra->mutex);
  assert(rv == 0);

  assert(ra->pointer_array[0] != NULL);

  if (ra->count == 0) {
    pthread_mutex_unlock(&ra->mutex);
    return NULL;
  }
  void * ret = ra->pointer_array[ra->tail];

  ra->tail = (ra->tail + 1) % ra->capacity;
  assert(ra->tail < ra->capacity);

  ra->count--;
  assert(ra->count >= 0);

  pthread_mutex_unlock(&ra->mutex);
  return ret;
}

#if defined(__cplusplus)
}
#endif

#endif //CUBEB_RING_ARRAY_H
