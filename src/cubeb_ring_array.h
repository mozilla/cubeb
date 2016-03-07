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
    asynchronous producer/consumer callbacks do not arrive in a
    repeated order the ring array stores the buffers and fetch
    them in the correct order. */
#define RING_ARRAY_CAPACITY 8

typedef struct {
  void* pointer_array[RING_ARRAY_CAPACITY];   /**< Array that hold pointers of the allocated space for the buffers. */
  unsigned int tail;                          /**< Index of the last element (first to deliver). */
  int count;                                  /**< Number of elements in the array. */
  unsigned int capacity;                      /**< Total length of the array. */
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

  assert(ra->pointer_array[0] == NULL);

  return 0;
}

/** Set the allocated space to store the data.
    This must be called after init and before the get operation buffer.
    @param ra The ring_array pointer.
    @param data Pointer to allocated space of buffers.
    @param index Index between 0 and capacity-1 to store the allocated data buffer. */
void
ring_array_set_data(ring_array * ra, void * data, unsigned int index)
{
  assert(index < ra->capacity);
  ra->pointer_array[index] = data;
}

/** Destroy the ring array.
    @param ra The ring_array pointer.*/
void
ring_array_destroy(ring_array * ra)
{
}

/** Get the allocated buffer to be stored with fresh data.
    @param ra The ring_array pointer.
    @retval Pointer of the allocated space to be stored with fresh data or NULL if full. */
void *
ring_array_get_next_free_buffer(ring_array * ra)
{
  assert(ra->pointer_array[0] != NULL);
  if (ra->count == (int)ra->capacity) {
    return NULL;
  }

  assert(ra->count == 0 || (ra->tail + ra->count) % ra->capacity != ra->tail);
  void * ret = ra->pointer_array[(ra->tail + ra->count) % ra->capacity];

  ++ra->count;
  assert(ra->count <= (int)ra->capacity);

  return ret;
}

/** Get the next available buffer with data.
    @param ra The ring_array pointer.
    @retval Pointer of the next in order data buffer or NULL if empty. */
void *
ring_array_get_first_data_buffer(ring_array * ra)
{
  assert(ra->pointer_array[0] != NULL);

  if (ra->count == 0) {
    return NULL;
  }
  void * ret = ra->pointer_array[ra->tail];

  ra->tail = (ra->tail + 1) % ra->capacity;
  assert(ra->tail < ra->capacity);

  ra->count--;
  assert(ra->count >= 0);

  return ret;
}

#if defined(__cplusplus)
}
#endif

#endif //CUBEB_RING_ARRAY_H
