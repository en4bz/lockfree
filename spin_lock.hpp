#pragma once

#include <atomic>
#include <utility>

class spin_lock {
    std::atomic_flag _flag = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (_flag.test_and_set(std::memory_order_acq_rel))
            asm volatile("pause");
    }

    bool try_lock() {
        return !_flag.test_and_set(std::memory_order_acq_rel);
    }

    void unlock() {
        _flag.clear(std::memory_order_release);
    }
};

class spin_lock_backoff {
    std::atomic_flag _flag = ATOMIC_FLAG_INIT;
public:
    void lock() {
        unsigned backoff = 0;
        while (_flag.test_and_set(std::memory_order_acq_rel)) {
            for(unsigned i = 0; i < (1u << backoff); i++) {
                asm volatile("pause");
            }
            backoff++;
        }
    }

    bool try_lock() {
        return !_flag.test_and_set(std::memory_order_acq_rel);
    }

    void unlock() {
        _flag.clear(std::memory_order_release);
    }
};

