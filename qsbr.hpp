#pragma once

#include <atomic>
#include <functional>

#include "mpsc_queue.hpp"

namespace policy {

struct Free {
  using storage_t = void*;

  static void free(storage_t ptr) {
    ::free(ptr);
  }

  static void free_array(storage_t ptr) {
    ::free(ptr);
  }
};

template<typename T>
struct Delete {
  using storage_t = T*;

  static void free(storage_t ptr) {
    delete ptr;
  }

  static void free_array(storage_t ptr) {
    delete [] ptr;
  }
};

struct Generic {
  using storage_t = std::function<void()>;

  static void free(storage_t& fn) {
    fn();
  }

  static void free_array(storage_t& fn) {
    fn();
  }
};

}

/**
 * Lock-free Quiescent State Based Reclamation.
 */
template<typename Policy>
class qsbr_impl {
  std::atomic<uint64_t> _counter;
  std::atomic<uint64_t> _quiescent;
  uint64_t _pad1[6];
  std::atomic<mpsc_queue<typename Policy::storage_t>*> _current;
  std::atomic<mpsc_queue<typename Policy::storage_t>*> _previous;
  uint64_t _pad2[6];

public:

  qsbr_impl() : _counter(0), _quiescent(0),
    _current( new mpsc_queue<typename Policy::storage_t>()),
    _previous(new mpsc_queue<typename Policy::storage_t>)
  {}

  // Cannot be called after any thread has called quiescent.
  // Can only register a maximum of upto 64 threads.
  uint64_t register_thread() {
    return _counter.fetch_add(1, std::memory_order_acq_rel);
  }

  template<typename U>
  void deferred_free(U&& ptr) {
    _current.load(std::memory_order_acquire)->push(std::forward<U>(ptr));
  }

  template<typename U>
  void deferred_free_array(U&& ptr) {
    _current.load(std::memory_order_acquire)->push(std::forward<U>(ptr));
  }

  void quiescent(const uint64_t tid) {
    const uint64_t mask = 1ul << tid;
    const uint64_t prev = _quiescent.fetch_or(mask, std::memory_order_acq_rel);
    if(prev != (prev | mask) && __builtin_popcountl(prev | mask) == _counter) {
      auto* const previous = _previous.load(std::memory_order_acquire);
      typename Policy::storage_t fn;
      while(previous->pop(fn))
        Policy::free(fn);

      auto* const temp = _current.exchange(_previous, std::memory_order_acq_rel);
      _previous.store(temp, std::memory_order_release);
      _quiescent.store(0, std::memory_order_release);
    }
  }

  ~qsbr_impl() {
    typename Policy::storage_t fn;
    while(_previous.load(std::memory_order_relaxed)->pop(fn))
      Policy::free(fn);
    while(_current.load(std::memory_order_relaxed)->pop(fn))
      Policy::free(fn);
  }
};

using qsbr = qsbr_impl<policy::Generic>;
