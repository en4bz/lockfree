#include "../mpsc_queue.hpp"

#include <thread>
#include <vector>
#include <iostream>
#include <cassert>
#include <string>
#include <unordered_set>

static mpsc_queue<std::string> rb;

void produce(char c) {
  for(int i = 1; i <= 10; i++)
      while(!rb.push(std::string(i,c)));
}

static std::unordered_set<std::string> seen;

void consume(long n) {
    while(n--) {
      std::string l;
      while(!rb.pop(l));
      seen.insert(l);
    }
}

int main() {
    std::vector<std::thread> threads;
    for(char c = 'a' ; c <= 'z'; c++)
      threads.emplace_back(produce, c);

    std::thread consumer(consume, 26 * 10);

    for(auto& t : threads)
      t.join();

    consumer.join();
    for(char c = 'a' ; c <= 'z'; c++) {
      for(int i = 1; i <= 10; i++) {
        assert(seen.count(std::string(i,c)));
        std::cout << std::string(i,c) << ',';
      }
      std::cout << std::endl;
    }


    std::string a;
    assert(!rb.pop(a));
    return 0;
}

