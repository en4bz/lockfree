#include "../mpsc_queue.hpp"

#include <thread>
#include <vector>
#include <iostream>
#include <cassert>

static mpsc_queue<long> rb;

void produce(long n) {
    while(n--) {
        while(!rb.push(n));
    }
}

void consume(long n, long* sum) {
    while(n--) {
        long l;
        while(!rb.pop(l));
        *sum += l;
    }
}

int main() {
    long sum = 0;
    const long n = 1024 * 1024;
    std::vector<std::thread> threads;
    for(int i = 0; i < 4; i++)
      threads.emplace_back(produce, n);

    std::thread consumer(consume, 4 * n, &sum);

    for(auto& t : threads)
      t.join();

    consumer.join();
    const long expected = 4 * n * (n-1) / 2;
    std::cout << expected << std::endl;
    std::cout << sum << std::endl;
    long a;
    assert(!rb.pop(a));
    return !(expected == sum);
}

