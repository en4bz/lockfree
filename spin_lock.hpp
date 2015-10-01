#pragma once

#include <atomic>
#include <utility>

class spin_lock {
    std::atomic_flag _flag = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (_flag.test_and_set(std::memory_order_acquire));
    }

    template <typename F, typename... Args>
    void lock(F func, Args... args) {
        while (_flag.test_and_set(std::memory_order_acquire)) {
            F(std::forward<Args...>(args...));
        }
    }

    bool try_lock() {
        return !_flag.test_and_set(std::memory_order_acquire);
    }

    void unlock() {
        _flag.clear(std::memory_order_release);
    }
};
