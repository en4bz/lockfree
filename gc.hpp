#pragma once

#include <atomic>
#include <functional>
#include <type_traits>

/**
 * Intrusive Hook Class
 */
struct collectable {
  std::atomic<collectable*> _next;

  collectable() : _next(nullptr) {}
  virtual ~collectable() {}
};

/**
 * Intrusive Queue
 */
class gc_queue {

  std::atomic<collectable*> _head;
  std::atomic<collectable*> _tail;

public:

  gc_queue() : _head(new collectable), _tail(_head.load()) {}

  void push(collectable* value) {
    collectable* old = _tail.exchange(value, std::memory_order_acq_rel);
    old->_next.store(value, std::memory_order_release);
  }

  void clear() {
    while(true) {
      collectable* head = _head.load(std::memory_order_relaxed);
      collectable* next = head->_next.load(std::memory_order_acquire);
      if(next) {
        _head.store(next, std::memory_order_relaxed);
        delete head;
        continue;
      }
      return;
    }
  }

  ~gc_queue() {
    clear();
    delete _head.load();
  }
};

/**
 * Lock-free Quiescent State Based Reclamation.
 */
class qsbr {
  
  struct deleter : public collectable {
    void*  ptr;

    deleter(void* p) : collectable(), ptr(p) {}

    ~deleter() {
      free(ptr);
    }
  };

  std::atomic<uint64_t> _counter;
  std::atomic<uint64_t> _quiescent;
  uint64_t _pad1[6];
  std::atomic<gc_queue*> _current;
  std::atomic<gc_queue*> _previous;
  uint64_t _pad2[6];

public:

  qsbr() : _counter(0), _quiescent(0),
     _current(new gc_queue),
    _previous(new gc_queue)
  {}

  // Cannot be called after any thread has called quiescent.
  // Can only register a maximum of upto 64 threads.
  uint64_t register_thread() {
    return _counter.fetch_add(1, std::memory_order_acq_rel);
  }

  void deferred_free(void* ptr) {
    _current.load(std::memory_order_acquire)->push(new deleter(ptr));
  }

  void deferred_delete(collectable* ptr) {
    _current.load(std::memory_order_acquire)->push(ptr);
  }

  //template<typename T>
  //void deferred_delete_array(T* ptr) {
  //  deleter d{ptr, func};
  //  _current.load(std::memory_order_acquire)->push(d);
  //}

  void quiescent(const uint64_t tid) {
    const uint64_t mask = 1ul << tid;
    const uint64_t prev = _quiescent.fetch_or(mask, std::memory_order_acq_rel);
    if(prev != (prev | mask) && __builtin_popcountl(prev | mask) == _counter) {
      auto* const previous = _previous.load(std::memory_order_acquire);
      previous->clear();

      auto* const temp = _current.exchange(_previous, std::memory_order_acq_rel);
      _previous.store(temp, std::memory_order_release);
      _quiescent.store(0, std::memory_order_release);
    }
  }

  ~qsbr() {
    _previous.load()->clear();
    _current.load()->clear();

    delete _previous.load();
    delete _current.load();
  }
};

