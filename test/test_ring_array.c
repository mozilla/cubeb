#include <pthread.h>
#include <assert.h>
#include <string.h>

#include "cubeb_ring_array.h"

int test_ring_array()
{
  ring_array ra;
  ring_array_init(&ra);
  int data[RING_ARRAY_CAPACITY] ;// {1,2,3,4,5,6,7,8};
  void * p_data = NULL;

  for (int i = 0; i < RING_ARRAY_CAPACITY; ++i) {
    data[i] = i; // in case RING_ARRAY_CAPACITY change value
    ring_array_set_data(&ra, &data[i], i);
  }

  /* Get store buffers*/
  for (int i = 0; i < RING_ARRAY_CAPACITY; ++i) {
    p_data = ring_array_store_buffer(&ra);
    assert(p_data == &data[i]);
  }
  /*Now array is full extra store should give NULL*/
  assert(NULL == ring_array_store_buffer(&ra));
  /* Get fetch buffers*/
  for (int i = 0; i < RING_ARRAY_CAPACITY; ++i) {
    p_data = ring_array_fetch_buffer(&ra);
    assert(p_data == &data[i]);
  }
  /*Now array is empty extra fetch should give NULL*/
  assert(NULL == ring_array_fetch_buffer(&ra));

  p_data = NULL;
  /* Repeated store fetch should can go for ever*/
  for (int i = 0; i < 2*RING_ARRAY_CAPACITY; ++i) {
    p_data = ring_array_store_buffer(&ra);
    assert(p_data);
    assert(ring_array_fetch_buffer(&ra) == p_data);
  }

  p_data = NULL;
  /* Verify/modify buffer data*/
  for (int i = 0; i < RING_ARRAY_CAPACITY; ++i) {
    p_data = ring_array_store_buffer(&ra);
    assert(p_data);
    assert(*((int*)p_data) == data[i]);
    (*((int*)p_data))++; // Modify data
  }
  for (int i = 0; i < RING_ARRAY_CAPACITY; ++i) {
    p_data = ring_array_fetch_buffer(&ra);
    assert(p_data);
    assert(*((int*)p_data) == i+1); // Verify modified data
  }

  ring_array_destroy(&ra);

  return 0;
}


int main()
{
  test_ring_array();
  return 0;
}

