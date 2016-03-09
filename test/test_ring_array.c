#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <CoreAudio/CoreAudioTypes.h>

#include "cubeb/cubeb.h"
#include "cubeb_ring_array.h"

int test_ring_array()
{
  ring_array ra;

  assert(ring_array_init(&ra, 0, 0, 1, 1) == CUBEB_ERROR_INVALID_PARAMETER);
  assert(ring_array_init(&ra, 1, 0, 0, 1) == CUBEB_ERROR_INVALID_PARAMETER);

  unsigned int capacity = 8;
  ring_array_init(&ra, capacity, sizeof(int), 1, 1);
  int verify_data[capacity] ;// {1,2,3,4,5,6,7,8};
  AudioBuffer * p_data = NULL;

  for (unsigned int i = 0; i < capacity; ++i) {
    verify_data[i] = i; // in case capacity change value
    *(int*)ra.buffer_array[i].mData = i;
    assert(ra.buffer_array[i].mDataByteSize == 1 * sizeof(int));
    assert(ra.buffer_array[i].mNumberChannels == 1);
  }

  /* Get store buffers*/
  for (unsigned int i = 0; i < capacity; ++i) {
    p_data = ring_array_get_free_buffer(&ra);
    assert(p_data && *(int*)p_data->mData == verify_data[i]);
  }
  /*Now array is full extra store should give NULL*/
  assert(NULL == ring_array_get_free_buffer(&ra));
  /* Get fetch buffers*/
  for (unsigned int i = 0; i < capacity; ++i) {
    p_data = ring_array_get_data_buffer(&ra);
    assert(p_data && *(int*)p_data->mData == verify_data[i]);
  }
  /*Now array is empty extra fetch should give NULL*/
  assert(NULL == ring_array_get_data_buffer(&ra));

  p_data = NULL;
  /* Repeated store fetch should can go for ever*/
  for (unsigned int i = 0; i < 2*capacity; ++i) {
    p_data = ring_array_get_free_buffer(&ra);
    assert(p_data);
    assert(ring_array_get_data_buffer(&ra) == p_data);
  }

  p_data = NULL;
  /* Verify/modify buffer data*/
  for (unsigned int i = 0; i < capacity; ++i) {
    p_data = ring_array_get_free_buffer(&ra);
    assert(p_data);
    assert(*((int*)p_data->mData) == verify_data[i]);
    (*((int*)p_data->mData))++; // Modify data
  }
  for (unsigned int i = 0; i < capacity; ++i) {
    p_data = ring_array_get_data_buffer(&ra);
    assert(p_data);
    assert(*((int*)p_data->mData) == verify_data[i]+1); // Verify modified data
  }

  ring_array_destroy(&ra);

  return 0;
}


int main()
{
  test_ring_array();
  return 0;
}

