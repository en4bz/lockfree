#include "qsbr.hpp"

#include <cstring>
#include <unistd.h>
#include <x86intrin.h>
#include <type_traits>

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
 */
template <typename T,
         size_t BUCKET_SIZE = 8,
         typename Hash = std::hash<T>,
         typename Equal = std::equal_to<T>>
class hash_set : private Hash, Equal {

  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially_copyable!");
  static_assert(std::is_trivially_destructible<T>::value, "T must be trivially_destructible!");
//  static constexpr size_t BUCKET_SIZE = 8;
  static constexpr uintptr_t LOCK_BIT = 0x01;

  struct slot {
    size_t _hash;
    T      _item;

    slot(size_t h = 0, T i = T()) : _hash(h), _item(i) {}
    slot(const slot& copy) = default;
  };

  struct bucket : private Equal {
    unsigned _size{0};
    slot     _items[BUCKET_SIZE];

    bucket() : _items() {}
    bucket(const bucket&) = default;

    int find(const T& value, const size_t hash) const {
      for(unsigned i = 0; i < _size; i++) {
        const slot& s = _items[i];
        if(s._hash == hash && Equal::operator()(s._item, value))
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

    void insert(const T value, const size_t hash) {
      _items[_size++] = slot{hash, value};
    }

    void remove(const int index) {
      remove_at(_items, index);
      --_size;
    }

    void reset() noexcept {
      _size = 0;
    }

    void clear() {
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
    auto* const buckets = new std::atomic<bucket*>[bcount];
    for(size_t i = 0; i < bcount; i++)
      buckets[i].store(new bucket());
    zip(buckets, bcount);
  }

  ~hash_set() {
    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(buckets, modulus);
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
    unzip(buckets, modulus);
    const bucket* const b = strip_lock(buckets[hash % modulus]);
    int result = b->find(value, hash);
    if(result == 4)
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
        qs.deferred_free(old);
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
    if(index >= 0 && !old->empty()) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      copy->remove(index);
      if(buckets[hash % modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
        qs.deferred_free(old);
      }
      else {
        return erase(value, tid, copy);
      }
    }
    else if(prealloc)
      delete prealloc;

    qs.quiescent(tid);
    return index >= 0;
  }

  bool rehash() {
    bool prev = _rehashing.exchange(true, std::memory_order_acq_rel);
    if(prev)
      return false; // someone is already rehashing
    else
      write(1, "rehash\n", 8);

    size_t modulus;
    std::atomic<bucket*>* buckets;
    unzip(buckets, modulus);

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
    zip(newb, modulus << 1);
    _rehashing.store(false, std::memory_order_release);
    return true;
  }
};
