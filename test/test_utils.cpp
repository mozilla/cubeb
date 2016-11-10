#include "gtest/gtest.h"
#include "cubeb_utils.h"

void test_auto_array()
{
  auto_array<uint32_t> array;
  auto_array<uint32_t> array2(10);
  uint32_t a[10];

  ASSERT_TRUE(array2.length() == 0);
  ASSERT_TRUE(array2.capacity() == 10);


  for (uint32_t i = 0; i < 10; i++) {
    a[i] = i;
  }

  ASSERT_TRUE(array.capacity() == 0);
  ASSERT_TRUE(array.length() == 0);

  array.push(a, 10);

  ASSERT_TRUE(!array.reserve(9));

  for (uint32_t i = 0; i < 10; i++) {
    ASSERT_TRUE(array.data()[i] == i);
  }

  ASSERT_TRUE(array.capacity() == 10);
  ASSERT_TRUE(array.length() == 10);

  uint32_t b[10];

  array.pop(b, 5);

  ASSERT_TRUE(array.capacity() == 10);
  ASSERT_TRUE(array.length() == 5);
  for (uint32_t i = 0; i < 5; i++) {
    ASSERT_TRUE(b[i] == i);
    ASSERT_TRUE(array.data()[i] == 5 + i);
  }
  uint32_t* bb = b + 5;
  array.pop(bb, 5);

  ASSERT_TRUE(array.capacity() == 10);
  ASSERT_TRUE(array.length() == 0);
  for (uint32_t i = 0; i < 5; i++) {
    ASSERT_TRUE(bb[i] == 5 + i);
  }

  ASSERT_TRUE(!array.pop(nullptr, 1));

  array.push(a, 10);
  array.push(a, 10);

  for (uint32_t j = 0; j < 2; j++) {
    for (uint32_t i = 0; i < 10; i++) {
      ASSERT_TRUE(array.data()[10 * j + i] == i);
    }
  }
  ASSERT_TRUE(array.length() == 20);
  ASSERT_TRUE(array.capacity() == 20);
  array.pop(nullptr, 5);

  for (uint32_t i = 0; i < 5; i++) {
    ASSERT_TRUE(array.data()[i] == 5 + i);
  }

  ASSERT_TRUE(array.length() == 15);
  ASSERT_TRUE(array.capacity() == 20);
}

TEST(utils, main)
{
  test_auto_array();
}
