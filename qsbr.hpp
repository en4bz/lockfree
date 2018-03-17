#pragma once

#include <atomic>
#include <functional>

#include "mpsc_queue.hpp"
/**
 * Lock-free Quiescent State Based Reclamation.
 */
class qsbr {
  std::atomic<uint64_t> _counter;
  std::atomic<uint64_t> _quiescent;
  uint64_t _pad1[6];
  std::atomic<mpsc_queue<std::function<void()>>*> _current;
  std::atomic<mpsc_queue<std::function<void()>>*> _previous;
  uint64_t _pad2[6];

public:

  qsbr() : _counter(0), _quiescent(0),
    _current(new mpsc_queue<std::function<void()>>()),
    _previous(new mpsc_queue<std::function<void()>>)
  {}

  // Cannot be called after any thread has called quiescent.
  // Can only register a maximum of upto 64 threads.
  uint64_t register_thread() {
    return _counter.fetch_add(1, std::memory_order_acq_rel);
  }

  template<typename T>
  void deferred_free(T* ptr) {
    // SBO should apply to the lambda below so no allocations.
    _current.load(std::memory_order_acquire)->push([ptr](){ delete ptr;});
  }

  template<typename T>
  void deferred_free_array(T* ptr) {
    _current.load(std::memory_order_acquire)->push([ptr](){ delete [] ptr;});
  }

  void quiescent(const uint64_t tid) {
    const uint64_t mask = 1ul << tid;
    const uint64_t prev = _quiescent.fetch_or(mask, std::memory_order_acq_rel);
    if(prev != (prev | mask) && __builtin_popcountl(prev | mask) == _counter) {
      auto* const previous = _previous.load(std::memory_order_acquire);
      std::function<void()> fn;
      while(previous->pop(fn))
        fn();

      auto* const temp = _current.exchange(_previous, std::memory_order_acq_rel);
      _previous.store(temp, std::memory_order_release);
      _quiescent.store(0, std::memory_order_release);
    }
  }

  ~qsbr() {
    std::function<void()> fn;
    while(_previous.load(std::memory_order_relaxed)->pop(fn))
      fn();
    while(_current.load(std::memory_order_relaxed)->pop(fn))
      fn();
  }
};
