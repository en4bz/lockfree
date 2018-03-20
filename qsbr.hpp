#pragma once

#include <atomic>
#include <functional>
#include <type_traits>

#include "mpsc_queue.hpp"

/**
 * Lock-free Quiescent State Based Reclamation.
 */
class qsbr {

  struct deleter {
    void*  ptr;
    void (*fn)(void*);

    void operator()() const {
      fn(ptr);
    }
  };

  std::atomic<uint64_t> _counter;
  std::atomic<uint64_t> _quiescent;
  uint64_t _pad1[6];
  std::atomic<mpsc_queue<deleter>*> _current;
  std::atomic<mpsc_queue<deleter>*> _previous;
  uint64_t _pad2[6];

public:

  qsbr() : _counter(0), _quiescent(0),
     _current(new mpsc_queue<deleter>),
    _previous(new mpsc_queue<deleter>)
  {}

  // Cannot be called after any thread has called quiescent.
  // Can only register a maximum of upto 64 threads.
  uint64_t register_thread() {
    return _counter.fetch_add(1, std::memory_order_acq_rel);
  }

  template<typename T>
  void deferred_free(T* ptr) {
    deleter d{ptr, ::free};
    _current.load(std::memory_order_acquire)->push(d);
  }

  template<typename T>
  void deferred_delete(T* ptr) {
    static const auto func = [](void* p) { delete static_cast<T*>(p); };
    deleter d{ptr, func};
    _current.load(std::memory_order_acquire)->push(d);
  }

  template<typename T>
  void deferred_delete_array(T* ptr) {
    static const auto func = [](void* p) { delete [] static_cast<T*>(p); };
    deleter d{ptr, func};
    _current.load(std::memory_order_acquire)->push(d);
  }

  void quiescent(const uint64_t tid) {
    const uint64_t mask = 1ul << tid;
    const uint64_t prev = _quiescent.fetch_or(mask, std::memory_order_acq_rel);
    if(prev != (prev | mask) && __builtin_popcountl(prev | mask) == _counter) {
      auto* const previous = _previous.load(std::memory_order_acquire);
      deleter d;
      while(previous->pop(d))
        d();

      auto* const temp = _current.exchange(_previous, std::memory_order_acq_rel);
      _previous.store(temp, std::memory_order_release);
      _quiescent.store(0, std::memory_order_release);
    }
  }

  ~qsbr() {
    deleter d;
    while(_previous.load(std::memory_order_relaxed)->pop(d))
      d();
    while(_current.load(std::memory_order_relaxed)->pop(d))
      d();

    delete _previous.load();
    delete _current.load();
  }
};
