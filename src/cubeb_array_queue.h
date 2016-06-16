/*
 * Copyright © 2016 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#ifndef CUBEB_ARRAY_QUEUE_H
#define CUBEB_ARRAY_QUEUE_H

#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct
{
  void ** buf;
  size_t num;
  size_t writePos;
  size_t readPos;
  pthread_mutex_t mutex;
  pthread_mutex_t empty_mutex;
} array_queue;

array_queue * array_queue_create(size_t num)
{
  assert(num > 0);

  array_queue * new_queue = (array_queue*)calloc(1, sizeof(array_queue));
  new_queue->buf = (void **)calloc(1, sizeof(void*) * num);
  new_queue->readPos = 0;
  new_queue->writePos = 0;
  new_queue->num = num;

  pthread_mutex_init(&new_queue->mutex, NULL);
  pthread_mutex_init(&new_queue->empty_mutex, NULL);
  pthread_mutex_lock(&new_queue->empty_mutex);

  return new_queue;
}

void array_queue_destroy(array_queue * aq)
{
  assert(aq);

  free(aq->buf);
  pthread_mutex_destroy(&aq->mutex);

  pthread_mutex_unlock(&aq->empty_mutex);
  pthread_mutex_destroy(&aq->empty_mutex);

  free(aq);
}

int array_queue_push(array_queue * aq, void * item)
{
  assert(item);

  pthread_mutex_lock(&aq->mutex);
  int ret = -1;
  if(aq->buf[aq->writePos % aq->num] == NULL)
  {
    aq->buf[aq->writePos % aq->num] = item;
    aq->writePos = (aq->writePos + 1) % aq->num;
    ret = 0;
  }
  // else queue is full
  pthread_mutex_unlock(&aq->empty_mutex);
  pthread_mutex_unlock(&aq->mutex);
  return ret;
}

void* array_queue_pop(array_queue * aq)
{
  pthread_mutex_lock(&aq->mutex);
  void * value = aq->buf[aq->readPos % aq->num];
  if(value)
  {
    aq->buf[aq->readPos % aq->num] = NULL;
    aq->readPos = (aq->readPos + 1) % aq->num;
    if (aq->readPos == aq->writePos) {
      pthread_mutex_lock(&aq->empty_mutex);
    }
  }
  pthread_mutex_unlock(&aq->mutex);
  return value;
}

void array_queue_wait_if_empty(array_queue * aq)
{
  while (pthread_mutex_trylock(&aq->empty_mutex) != 0) {
    usleep(10);
  }
  pthread_mutex_unlock(&aq->empty_mutex);
}

ssize_t array_queue_get_size(array_queue * aq)
{
  pthread_mutex_lock(&aq->mutex);
  ssize_t r = aq->writePos - aq->readPos;
  if (r < 0) {
    r = aq->num + r;
  }
  pthread_mutex_unlock(&aq->mutex);
  return r;
}

#if defined(__cplusplus)
}
#endif

#endif //CUBE_ARRAY_QUEUE_H
