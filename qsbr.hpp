#pragma once

#include <atomic>
#include <functional>
#include <type_traits>

#include "mpsc_queue.hpp"

namespace detail {

template<typename T, bool b = std::is_array<T>::value>
struct Delete;

template<typename T>
struct Delete<T, false> {
  static void free(void * ptr) {
    delete static_cast<T*>(ptr);
  }
};

template<typename T>
struct Delete<T, true> {
  static void free(void * ptr) {
    delete [] static_cast<T*>(ptr);
  }
};

template <typename T, typename... Ts>
struct Index;

template <typename T, typename... Ts>
struct Index<T, T, Ts...> : std::integral_constant<std::size_t, 0> {};

template <typename T, typename U, typename... Ts>
struct Index<T, U, Ts...> : std::integral_constant<std::size_t, 1 + Index<T, Ts...>::value> {};

}

/**
 * Lock-free Quiescent State Based Reclamation.
 */
template<typename... Args>
class qsbr {

  struct deleter {
    void*  ptr;
    size_t index;
  };

  void do_delete(const deleter& d) {
    static void(* const deleters[])(void*) = {::free, detail::Delete<Args>::free...};
    deleters[d.index](d.ptr);
  }

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
    deleter d{ptr, 0};
    _current.load(std::memory_order_acquire)->push(d);
  }

  template<typename T>
  void deferred_delete(T* ptr) {
    deleter d{ptr, detail::Index<T, Args...>::value + 1};
    _current.load(std::memory_order_acquire)->push(d);
  }

  template<typename T>
  void deferred_delete_array(T* ptr) {
    deleter d{ptr, detail::Index<T[], Args...>::value + 1};
    _current.load(std::memory_order_acquire)->push(d);
  }

  void quiescent(const uint64_t tid) {
    const uint64_t mask = 1ul << tid;
    const uint64_t prev = _quiescent.fetch_or(mask, std::memory_order_acq_rel);
    if(prev != (prev | mask) && __builtin_popcountl(prev | mask) == _counter) {
      auto* const previous = _previous.load(std::memory_order_acquire);
      deleter d;
      while(previous->pop(d))
        do_delete(d);

      auto* const temp = _current.exchange(_previous, std::memory_order_acq_rel);
      _previous.store(temp, std::memory_order_release);
      _quiescent.store(0, std::memory_order_release);
    }
  }

  ~qsbr() {
    deleter d;
    while(_previous.load(std::memory_order_relaxed)->pop(d))
      do_delete(d);
    while(_current.load(std::memory_order_relaxed)->pop(d))
      do_delete(d);

    delete _previous.load();
    delete _current.load();
  }
};
