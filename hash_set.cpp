#include "qsbr.hpp"

#include <cstring>

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

  struct bucket : private Equal {
    int _size{0};

    struct slot {
      size_t _hash;
      T*     _item;

      slot(size_t h = 0, T* i = nullptr) : _hash(h), _item(i) {}
    } _items[8];

    bucket() : _items() {}
    bucket(const bucket&) = default;

    int find(const T& value, const size_t hash) const {
      for(int i = 0; i < _size; i++) {
        const slot& s = _items[i];
        if(s._hash == hash && s._item && Equal::operator()(*s._item, value))
          return i;
      }
      return -1;
    }

    bool full() const noexcept {
      return _size == 8;
    }

    bool empty() const noexcept {
      return _size == 0;
    }

    T* insert(const T& value, const size_t hash) {
      T* v = new T(value);
      _items[_size++] = slot{hash, v};
      return v;
    }

    T* remove(const int index) {
      T* old = _items[index]._item;
      remove_at(_items, index);
      _items[--_size] = slot();
      return old;
    }

    void clear() {
      for(int i = 0; i < _size; i++) {
        const slot& s = _items[i];
        delete s._item;
      }
    }
  };

public:

  mutable qsbr qs;

  std::atomic<std::atomic<bucket*>*> _buckets;
  std::atomic<std::size_t>           _modulus;

  hash_set(size_t bcount = 16) : _modulus(bcount) {
    _buckets.store(new std::atomic<bucket*>[bcount]);
    for(int i = 0; i < bcount; i++)
      _buckets[i].store(new bucket());
  }

  ~hash_set() {
    for(int i = 0; i < 16; i++) {
      _buckets[i].load()->clear();
      delete _buckets[i].load();
    }
    delete [] _buckets.load();
  }

  bool find(const T& value, const uint64_t tid) const {
    const size_t hash = Hash::operator()(value);
    const bucket* const b = _buckets[hash % _modulus].load(std::memory_order_acquire);
    int result = b->find(value, hash);
    qs.quiescent(tid);
    return result > 0;
  }

  bool insert(const T& value, const uint64_t tid, bucket* prealloc = nullptr) {
    const size_t hash = Hash::operator()(value);
    bucket* old = _buckets[hash % _modulus].load(std::memory_order_acquire);
    const int index = old->find(value, hash);
    if(index == -1 && !old->full()) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      T* const new_elem = copy->insert(value, hash);
      if(_buckets[hash % _modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
        qs.deferred_free(old);
      }
      else {
        delete new_elem; // TODO: reuse this
        return insert(value, tid, copy);
      }
    }
    else if(prealloc)
      delete prealloc;

    qs.quiescent(tid);
    return false;
  }

  bool erase(const T& value, const uint64_t tid, bucket* prealloc = nullptr) {
    const size_t hash = Hash::operator()(value);
    bucket* old = _buckets[hash % _modulus].load(std::memory_order_acquire);
    const int index = old->find(value, hash);
    if(index > 0 && !old->empty()) {
      // copy bucket
      bucket* copy = prealloc ? new (prealloc) bucket(*old) : new bucket(*old);
      T* old_elem = copy->remove(index);
      if(_buckets[hash % _modulus].compare_exchange_strong(old, copy, std::memory_order_acq_rel)) {
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
};

#include <random>
#include <vector>
#include <iostream>
#include <thread>

static hash_set<long> sss;

std::atomic_int spin(0);
std::atomic_int found(0);

void foo(const int seed) {
  const int N = 1000000;
  const uint64_t tid = sss.qs.register_thread();
  spin--;
  while(spin.load());

  if(tid % 2) {
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
