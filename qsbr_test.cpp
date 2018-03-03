#include "qsbr.hpp"

/**
 * Fixed size lock-free Hash Set that overwrites on insert.
 */
template<size_t N>
struct simple_int_set {

  qsbr qs;

  std::atomic<int*> _data[N];

  simple_int_set() {
    for(int i = 0; i < N; i++)
      _data[i] = nullptr;
  }

  size_t size() const noexcept {
    return N;
  }

  bool insert(int x, uint64_t tid) {
    int* ptr = new int(x);

    bool found = false;
    const uint64_t idx = x % size();
    int* slot = _data[idx].load(std::memory_order_acquire);
    found = slot && x == *slot;
    if(_data[idx].compare_exchange_strong(slot, ptr, std::memory_order_acq_rel))
      qs.deferred_free(slot);
    else
      delete ptr;
    qs.quiescent(tid);
    return found;
  }

  bool remove(int x, uint64_t tid) {
    const uint64_t idx = x % size();
    int* slot = _data[idx].load(std::memory_order_acquire);
    if(slot && x == *slot) {
      if(_data[idx].compare_exchange_strong(slot, nullptr, std::memory_order_acq_rel))
        qs.deferred_free(slot);
    }

    qs.quiescent(tid);
    return true;
  }

  bool find(int x, uint64_t tid) {
    const uint64_t idx = x % size();
    int* slot = _data[idx].load(std::memory_order_acquire);
    bool found = slot && *slot == x;
    qs.quiescent(tid);
    return found;
  }

  ~simple_int_set() {
    for(auto& ptr : _data)
      delete ptr.load();

  }
};

simple_int_set<16> sss;

#include <random>
#include <vector>
#include <iostream>
#include <thread>

std::atomic_int spin(0);
std::atomic_int found(0);

void foo(int seed) {
  spin--;
  const uint64_t tid = sss.qs.register_thread();
  while(spin.load());

  if(tid % 2) {
    for(int i = 0; i < 1000000; i++)
      sss.insert(i, tid);
    for(int i = 0; i < 1000000 ; i++)
      found += sss.find(i, tid);
  }
  else {
    for(int i = 0; i < 1000000 ; i++)
      found += sss.find(i, tid);
    for(int i = 0; i < 1000000; i++)
      sss.remove(i, tid);
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

