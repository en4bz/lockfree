#pragma once

#include "qsbr.hpp"

#include <cstring>
#include <type_traits>

template<typename T, size_t N>
static void remove_at(T(&arr)[N], size_t index) {
  std::memmove(arr + index, arr + index + 1, sizeof(T) * (N - index - 1));
}

/**
 * Bucket based hash set with CoW buckets.
 */
template <typename T,
         unsigned BUCKET_SIZE = 8,
         typename Hash = std::hash<T>,
         typename Equal = std::equal_to<T>>
class hash_set : private Hash, Equal {

  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially_copyable!");
  static_assert(std::is_trivially_destructible<T>::value, "T must be trivially_destructible!");

  static constexpr uintptr_t LOCK_BIT = 0x01;

  struct slot {
    size_t _hash;
    T      _item;

    slot() = delete;
    slot(size_t h = 0, T i = T()) : _hash(h), _item(i) {}
    slot(const slot& copy) = default;
  };

  struct bucket : private Equal {
    unsigned _size = 0;
    typename std::aligned_storage<sizeof(slot), alignof(slot)>::type _items[BUCKET_SIZE];

    bucket() = default;
    bucket(const bucket&) = default;

    int find(const T& value, const size_t hash) const {
      for(unsigned i = 0; i < _size; i++) {
        const slot& s = this->operator[](i);
        if(s._hash == hash && Equal::operator()(s._item, value))
          return i;
      }
      return -1;
    }

    const slot& operator[](const unsigned index) const noexcept {
      return *reinterpret_cast<const slot*>(_items + index);
    }

    bool full() const noexcept {
      return _size == BUCKET_SIZE;
    }

    bool empty() const noexcept {
      return _size == 0;
    }

    void insert(const slot& s) {
      new (_items + _size++) slot(s);
    }

    void insert(const T& value, const size_t hash) {
      new (_items + _size++) slot(hash, value);
    }

    void remove(const int index) {
      remove_at(_items, index);
      --_size;
    }

  };

  static_assert(std::is_trivially_copyable<bucket>::value, "Bucket is not TC!");

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

  /**
   * Creates a compressed pointer containing the log2 of the
   * modulus and the actual pointer to the bucket array.
   * -------------------------------------------------------
   * 63 free 56 log2(modulus) 48      pointer       2 free 0
   * -------------------------------------------------------
   */
  void zip(const void* const ptr, const size_t modulus) {
    uintptr_t top = (63ul - __builtin_clzl(modulus)) << 48;
    top |= reinterpret_cast<uintptr_t>(ptr);
    _top.store(top, std::memory_order_release);
  }

  void unzip(std::atomic<bucket*>*& ptr , size_t& modulus) const {
    uintptr_t top = _top.load(std::memory_order_acquire);
    modulus = 1ul << (top >> 48);
    ptr = reinterpret_cast<std::atomic<bucket*>*>(top & ~(0xFFFFul << 48));
  }

public:

  mutable qsbr qs;

  std::atomic_bool      _rehashing;
  std::atomic_uintptr_t _top;

  hash_set(size_t bcount = 16) {
    using bucket_ptr_t = std::atomic<bucket*>*;
    auto* const buckets = static_cast<bucket_ptr_t>(std::calloc(sizeof(bucket_ptr_t), bcount));
    for(size_t i = 0; i < bcount; i++)
      buckets[i].store(new bucket);
    zip(buckets, bcount);
  }

  ~hash_set() {
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(buckets, modulus);
    for(size_t i = 0; i < modulus; i++) {
      delete buckets[i].load();
    }
    free(buckets);
  }

  // If nonblocking is true this function is wait-free
  bool find(const T& value, const uint64_t tid, const bool nonblocking = true) const {
    const size_t  hash = Hash::operator()(value);
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(buckets, modulus);
    const bucket* const b = strip_lock(buckets[hash % modulus]);
    int result = b->find(value, hash);
    if(!nonblocking)
      qs.quiescent(tid);
    return result >= 0;
  }

  bool insert(const T& value, const uint64_t tid, bucket* prealloc = nullptr) {
    while(_rehashing.load(std::memory_order_acquire))
      asm("pause");
    const size_t hash = Hash::operator()(value);
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(buckets, modulus);
    bucket* old = strip_lock(buckets[hash % modulus]);
    const int index = old->find(value, hash);
    if(index == -1 && !old->full()) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      copy->insert(value, hash);
      if(buckets[hash % modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
        qs.deferred_delete(old);
      }
      else {
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
    unzip(buckets, modulus);
    bucket* old = strip_lock(buckets[hash % modulus]);
    const int index = old->find(value, hash);
    if(index >= 0) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      copy->remove(index);
      if(buckets[hash % modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
        qs.deferred_delete(old);
      }
      else {
        return erase(value, tid, copy);
      }
    }
    else
      delete prealloc;

    qs.quiescent(tid);
    return index >= 0;
  }

  bool rehash() {
    bool prev = _rehashing.exchange(true, std::memory_order_acq_rel);
    if(prev)
      return false; // someone is already rehashing

    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(buckets, modulus);

    using bucket_ptr_t = std::atomic<bucket*>*;
    auto* const newb = static_cast<bucket_ptr_t>(std::calloc(sizeof(bucket_ptr_t), modulus << 1));
    for(size_t i = 0; i < (modulus << 1); i++) {
      newb[i].store(new bucket, std::memory_order_relaxed);
    }
    for(size_t i = 0; i < modulus; i++) {
      // This "lock" ensures pending erasures/insertions are either
      // observered by this thread or fail.
      bucket* const b = lock(buckets[i]);
      for(size_t j = 0; j < b->_size ; j++) {
        const slot& oldslot = b->operator[](j);
        bucket& newbucket = *newb[oldslot._hash % (modulus << 1)].load();
        if(newbucket.full())
          throw 0; //TODO: Try Again
        else
          newbucket.insert(oldslot);
      }
      qs.deferred_delete(reinterpret_cast<bucket*>(reinterpret_cast<uintptr_t>(b) & ~LOCK_BIT));
    }
    qs.deferred_free(buckets);
    zip(newb, modulus << 1);
    _rehashing.store(false, std::memory_order_release);
    return true;
  }
};
