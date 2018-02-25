#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <iostream>

struct mlock {
  std::atomic<mlock*> _next;
  std::atomic_bool  _locked;

  mlock() : _next(nullptr), _locked(true) {}

  void reset() {
    _next.store(nullptr, std::memory_order_relaxed);
    _locked.store(true,  std::memory_order_relaxed);
  }  
};

class mcs_lock {
  std::atomic<mlock*>   _tail;
public:

  mcs_lock() : _tail(nullptr) {}

  void lock(mlock& m) {
    mlock* old = _tail.exchange(&m);
    if(old) {
      m.reset();
      old->_next.store(&m, std::memory_order_release);
      while(m._locked.load(std::memory_order_acquire))
        asm("pause");
    }
  }

  void unlock(mlock& m) {
    mlock* expected = &m;
    if(!_tail.compare_exchange_strong(expected, nullptr)) {
      while(!m._next.load(std::memory_order_acquire)) // next is a nullptr
        asm("pause");
      m._next.load(std::memory_order_relaxed)->_locked.store(false);
    }
  }
};

template<size_t TagNumber = 0>
class easy_mcs_lock : private mcs_lock {
  static thread_local mlock m;
public:

  void lock() {
    mcs_lock::lock(m);
  }

  void unlock() {
    mcs_lock::unlock(m);
  }
};

template<size_t TagNumber>
thread_local mlock easy_mcs_lock<TagNumber>::m;

static easy_mcs_lock<> lock;
static std::set<int>   nums;


void foo(const int i) {
  lock.lock();
  nums.insert(i);
  lock.unlock();
  lock.lock();
  nums.erase(nums.find(i));
  lock.unlock();
}


int main(int argc, char** argv) {
  if(argc != 2)
    return 2;

  std::vector<std::thread> threads;
  for(int i = 0; i < atoi(argv[1]); i++) {
    threads.emplace_back(foo, i);
  }

  for(auto& t : threads)
    t.join();

  for(int i : nums)
    std::cout << i << ',';
  std::cout << std::endl;
  return errno;
}
