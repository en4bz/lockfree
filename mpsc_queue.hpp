#pragma once

#include <atomic>
#include <utility>

template <typename T>
class mpsc_queue {

  struct node {
    std::atomic<node*> _next;
    T _value;

    node(const T& copy = T()) : _next(nullptr), _value(copy) {}
    node(T&& movable) : _next(nullptr), _value(std::move(movable)) {}
  };

  std::atomic<node*> _head;
  std::atomic<node*> _tail;

public:

  mpsc_queue() : _head(new node), _tail(_head.load()) {}

  bool push(const T& value) {
    node* n = new node(value);
    node* old = _tail.exchange(n, std::memory_order_acq_rel);
    old->_next.store(n, std::memory_order_release);
    return true;
  }

  bool push(T&& value) {
    node* n = new node(std::move(value));
    node* old = _tail.exchange(n, std::memory_order_acq_rel);
    old->_next.store(n, std::memory_order_release);
    return true;
  }

  bool pop(T& value) {
    node* head = _head.load(std::memory_order_relaxed);
    node* next = head->_next.load(std::memory_order_acquire);
    if(next) {
      _head.store(next, std::memory_order_relaxed);
      delete head;
      value = std::move(next->_value);
      return true;
    }
    return false;
  }

  ~mpsc_queue() {
    delete _head.load();
  }
};
