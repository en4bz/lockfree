#pragma once

#include <atomic>
#include <memory>

template<typename T, typename Allocator = std::allocator<T>>
class spsc_queue : private Allocator {
    std::atomic<uint64_t> _head;
    uint64_t _padding[7]; // for x86
    std::atomic<uint64_t> _tail;
    const uint64_t _len;
    const uint64_t _mask; 
    T* const _data;
public:
    spsc_queue(const uint64_t size)
        : _head(0), _tail(0), _len(size), _mask(_len - 1),
            _data(Allocator::allocate(size)) {}

    T& front() {
        return _data[_head & _mask];
    }
    
    T& back() {
        return _data[_tail & _mask];
    }

    size_t size() const {
        return _tail.load(std::memory_order_acquire) - _head.load(std::memory_order_acquire);
    }

    size_t capacity() const {
        return _len;
    }

    bool pop(T& elem) {
        const uint64_t head_idx = _head.load(std::memory_order_relaxed);
        const uint64_t tail_idx = _tail.load(std::memory_order_acquire);
        if(head_idx == tail_idx) {
            return false;
        } 
        else {
            elem = std::move(_data[head_idx & _mask]);
            _data[head_idx & _mask].~T(); // call dtor
            _head.store(head_idx + 1, std::memory_order_release);
            return true;
        }
    }

    bool push(const T& elem) {
        const uint64_t tail_idx = _tail.load(std::memory_order_relaxed);
        const uint64_t head_idx = _head.load(std::memory_order_acquire);
        if((tail_idx - _len) == head_idx) {
            return false; 
        }
        else {
            new (&_data[tail_idx & _mask]) T(elem);
            _tail.store(tail_idx + 1, std::memory_order_release);
            return true;
        }
    }

    ~spsc_queue() {
        uint64_t tail_idx = _tail.load(std::memory_order_acquire);
        uint64_t head_idx = _head.load(std::memory_order_acquire);
        while(head_idx++ < tail_idx) {
            _data[head_idx & _mask].~T(); // destruct remaining elements
        }
        Allocator::deallocate(_data, _len);
    }
};
