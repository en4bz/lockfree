#include "../hash_set.hpp"
#include <gtest/gtest.h>

TEST(HashSet, Simple) {
  hash_set<int> hs;

  const uint64_t tid = hs.qs.register_thread();

  hs.insert(5, tid);
  ASSERT_TRUE(hs.find(5, tid));
  hs.erase(5, tid);
  ASSERT_FALSE(hs.find(5, tid));
  ASSERT_FALSE(hs.find(0, tid));

  for(int i = 0; i < 10; i++) {
    ASSERT_FALSE(hs.erase(i, tid)); // erase non-existing
    hs.insert(i, tid);
  }

  for(int i = 0; i < 10; i++) {
    ASSERT_TRUE(hs.find(i, tid));
    ASSERT_FALSE(hs.insert(i, tid)); // already exists
  }
 
  for(int i = 0; i < 10; i++) {
    ASSERT_TRUE(hs.erase(i, tid));
    ASSERT_FALSE(hs.find(i, tid)); // gone
  }
}

TEST(HashSet, ReHash) {
  hash_set<int> hs;

  const uint64_t tid = hs.qs.register_thread();

  hs.insert(5, tid);
  ASSERT_TRUE(hs.find(5, tid));
  hs.rehash();
  ASSERT_TRUE(hs.find(5, tid));
  hs.rehash();
  ASSERT_TRUE(hs.find(5, tid));
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
};
