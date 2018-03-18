#include "../hash_set.hpp"

#include <random>
#include <vector>
#include <thread>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <x86intrin.h>

static hash_set<long> sss(1 << 16);

static std::atomic_int spin(0);
static volatile int found(0);

using namespace std::chrono;

void foo(const int seed) {
  const int N = 100000000;
  const uint64_t tid = sss.qs.register_thread();
  spin--;
  while(spin.load());
  
  std::mt19937 gen(seed);
  std::uniform_int_distribution<int> rand(0,100);

  long sumf = 0;
  double countf = 0;

  long sumi = 0;
  double counti = 0;

  long sume = 0;
  double counte = 0;

  for(int i = 0; i < N; i++) {
    const int a = rand(gen);
    const long start = __rdtsc();
    if(a < 80) {
      found += sss.find(i, tid);
      sumf += __rdtsc() - start;
      countf += 1.0;
    }
    else if(a < 90) {
      sss.insert(i, tid);
      sumi += __rdtsc() - start;
      counti += 1.0;
    }
    else {
      sss.erase(i, tid);
      sume += __rdtsc() - start;
      counte += 1.0;
    }
  }
  std::cout << std::setprecision(6) << std::right << sumf / countf / 3.5 << ' ' << sumi / counti / 3.5 << ' ' << sume / counte / 3.5 << std::endl;
}

int main(int argc, char** argv) {
  if(argc != 2)
    return 2;

  const int n = atoi(argv[1]);
  spin.store(n);
  std::vector<std::thread> threads;
  for(int i = 1; i <= n; i++) {
    threads.emplace_back(foo, i);
  }
  for(auto& t : threads)
    t.join();
  std::cout << found << std::endl;
}
