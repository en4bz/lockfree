#pragma once

#include <atomic>

template <typename T>
class mpsc_queue {

  struct node {
    std::atomic<node*> _next;
    T _value;

    node(const T& copy = T()) : _next(nullptr), _value(copy) {}
  };

  std::atomic<node*> _head;
  std::atomic<node*> _tail;

public:

  mpsc_queue() : _head(new node), _tail(_head.load()) {}

  void push(const T& value) {
    node* n = new node(value);
    node* old = _tail.exchange(n, std::memory_order_acq_rel);
    old->_next.store(n, std::memory_order_release);
  }

  bool pop(T& value) {
    node* head = _head.load(std::memory_order_acquire);
    node* next = head->_next.load(std::memory_order_acquire);
    if(next) {
      _head.store(next);
      delete head;
      value = next->_value;
      return true;
    }
    return false;
  }

};
