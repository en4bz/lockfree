#include "qsbr.hpp"

#include <cstring>
#include <unistd.h>
#include <x86intrin.h>
#include <iostream>

template<typename T, size_t N>
static void insert_at(T(&arr)[N], size_t index, T value) {
  std::memmove(arr + index + 1, arr + index, sizeof(T) * (N - index - 1));
  arr[index] = value;
}

template<typename T, size_t N>
static void remove_at(T(&arr)[N], size_t index) {
  std::memmove(arr + index, arr + index + 1, sizeof(T) * (N - index - 1));
}

/**
 * Bucket based hash set with CoW buckets.
 * No rehashing support yet.
 */
template <typename T,
         typename Hash = std::hash<T>,
         typename Equal = std::equal_to<T>>
class hash_set : private Hash, Equal {

  static constexpr size_t BUCKET_SIZE = 8;
  static constexpr uintptr_t LOCK_BIT = 0x01;

  struct slot {
    size_t _hash;
    T*     _item;

    slot(size_t h = 0, T* i = nullptr) : _hash(h), _item(i) {}
    slot(const slot& copy) = default;
  };

  struct bucket : private Equal {
    size_t _size{0};
    slot   _items[BUCKET_SIZE];

    bucket() : _items() {}
    bucket(const bucket&) = default;

    int find(const T& value, const size_t hash) const {
      for(size_t i = 0; i < _size; i++) {
        const slot& s = _items[i];
        if(s._hash == hash && s._item && Equal::operator()(*s._item, value))
          return i;
      }
      return -1;
    }

    bool full() const noexcept {
      return _size == BUCKET_SIZE;
    }

    bool empty() const noexcept {
      return _size == 0;
    }

    void insert(const slot& s) noexcept {
      _items[_size++] = s;
    }

    T* insert(T* const value, const size_t hash) {
      _items[_size++] = slot{hash, value};
      return value;
    }

    T* insert(const T& value, const size_t hash) {
      T* v = new T(value);
      return insert(v, hash);
    }

    T* remove(const int index) {
      T* old = _items[index]._item;
      remove_at(_items, index);
      _items[--_size] = slot();
      return old;
    }

    void reset() noexcept {
      _size = 0;
    }

    void clear() {
      for(size_t i = 0; i < _size; i++) {
        const slot& s = _items[i];
        delete s._item;
      }
      _size = 0;
    }
  };

  /**
   * Locks a bucket ensuring all further CAS operations fail.
   */
  static bucket* lock(std::atomic<bucket*>& ptr) {
    const uintptr_t result = reinterpret_cast<std::atomic<uintptr_t>*>(&ptr)->fetch_or(LOCK_BIT, std::memory_order_acq_rel);
    return reinterpret_cast<bucket*>(result);
  }

  static bucket* strip_lock(const std::atomic<bucket*>& ptr) {
    return reinterpret_cast<bucket*>(reinterpret_cast<uintptr_t>(ptr.load(std::memory_order_acquire)) & ~LOCK_BIT);
  }

  static uintptr_t zip(const void* const ptr, const size_t modulus) {
    uintptr_t top = (63ul - __builtin_clzl(modulus)) << 48;
    top |= reinterpret_cast<uintptr_t>(ptr);
    return top;
  }

  static void unzip(uintptr_t top, std::atomic<bucket*>*& ptr , size_t& modulus) {
    modulus = 1ul << (top >> 48);
    ptr = reinterpret_cast<std::atomic<bucket*>*>(top & ~(0xFFFFul << 48));
  }

public:

  mutable qsbr qs;

  std::atomic_bool      _rehashing;
  std::atomic_uintptr_t _top;

  hash_set(size_t bcount = 16) {
    auto* const buckets = new std::atomic<bucket*>[bcount];
    for(size_t i = 0; i < bcount; i++)
      buckets[i].store(new bucket());
    uintptr_t top = zip(buckets, bcount);
    _top.store(top, std::memory_order_relaxed);
  }

  ~hash_set() {
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(_top, buckets, modulus);
    for(size_t i = 0; i < modulus; i++) {
      buckets[i].load()->clear();
      delete buckets[i].load();
    }
    delete [] buckets;
  }

  bool find(const T& value, const uint64_t tid) const {
    const size_t  hash = Hash::operator()(value);
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(_top.load(std::memory_order_acquire), buckets, modulus);
    const bucket* const b = strip_lock(buckets[hash % modulus]);
    int result = b->find(value, hash);
    qs.quiescent(tid);
    return result >= 0;
  }

  bool insert(const T& value, const uint64_t tid, bucket* prealloc = nullptr) {
    while(_rehashing.load(std::memory_order_acquire))
      asm("pause");
    const size_t hash = Hash::operator()(value);
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(_top.load(std::memory_order_acquire), buckets, modulus);
    bucket* old = strip_lock(buckets[hash % modulus]);
    const int index = old->find(value, hash);
    if(index == -1 && !old->full()) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      T* const new_elem = copy->insert(value, hash);
      if(buckets[hash % modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
        qs.deferred_free(old);
      }
      else {
        delete new_elem; // TODO: reuse this
        return insert(value, tid, copy);
      }
    }
    else if(old->full()) {
      rehash();
      return insert(value, tid, prealloc);
    }
    else
      delete prealloc;

    qs.quiescent(tid);
    return false;
  }

  bool erase(const T& value, const uint64_t tid, bucket* prealloc = nullptr) {
    while(_rehashing.load(std::memory_order_acquire))
      asm("pause");
    const size_t hash = Hash::operator()(value);
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(_top.load(std::memory_order_acquire), buckets, modulus);
    bucket* old = strip_lock(buckets[hash % modulus]);
    const int index = old->find(value, hash);
    if(index > 0 && !old->empty()) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      T* old_elem = copy->remove(index);
      if(buckets[hash % modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
        qs.deferred_free(old);
        qs.deferred_free(old_elem);
      }
      else {
        return erase(value, tid, copy);
      }
    }
    else if(prealloc)
      delete prealloc;

    qs.quiescent(tid);
    return index > 0;
  }

  bool rehash() {
    bool prev = _rehashing.exchange(true, std::memory_order_acq_rel);
    if(prev)
      return false; // someone is already rehashing
    else
      write(1, "rehash\n", 8);

    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(_top.load(std::memory_order_acquire), buckets, modulus);

    std::atomic<bucket*>* newb  = new std::atomic<bucket*>[modulus << 1];
    for(size_t i = 0; i < (modulus << 1); i++) {
      newb[i].store(new bucket, std::memory_order_relaxed);
    }
    for(size_t i = 0; i < modulus; i++) {
      // This "lock" ensures pending erasures/insertions are either
      // observered by this thread or fail.
      bucket* const b = lock(buckets[i]);
      for(size_t j = 0; j < b->_size ; j++) {
        slot& oldslot = b->_items[j];
        bucket& newbucket = *newb[oldslot._hash % (modulus << 1)].load();
        if(newbucket.full())
          throw 0; //TODO: Try Again
        else
          newbucket.insert(oldslot);
      }
      qs.deferred_free(reinterpret_cast<bucket*>(reinterpret_cast<uintptr_t>(b) & ~LOCK_BIT));
    }
    qs.deferred_free_array(buckets);
    _top.store(zip(newb, modulus << 1), std::memory_order_release);
    _rehashing.store(false, std::memory_order_release);
    return true;
  }
};

#include <random>
#include <vector>
#include <thread>

static hash_set<long> sss;

std::atomic_int spin(0);
std::atomic_int found(0);

void foo(const int seed) {
  const int N = 1000000;
  const uint64_t tid = sss.qs.register_thread();
  spin--;
  while(spin.load());

  if(tid % 2 == 0) {
    for(int i = 0; i < N; i++)
      sss.insert(i, tid);
    for(int i = 0; i < N; i++)
      found += sss.find(i, tid);
  }
  else {
    for(int i = 0; i < N; i++)
      found += sss.find(i, tid);
    for(int i = 0; i < N; i++)
      sss.erase(i, tid);
  }
}

int main(int argc, char** argv) {
  if(argc != 2)
    return 2;

  const int n = atoi(argv[1]);
  spin.store(n);
  std::vector<std::thread> threads;
  for(int i = 1; i <= n; i++) {
    threads.emplace_back(foo, i);
  }
  for(auto& t : threads)
    t.join();
  std::cout << found << std::endl;
}
