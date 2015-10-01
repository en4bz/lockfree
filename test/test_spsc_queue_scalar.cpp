#include <iostream>
#include <thread>

#include "../spsc_queue.hpp"

spsc_queue<int> rb(1024);

void produce(long n) {
    while(n--) {
        while(!rb.push(n));
    } 
}

void consume(long n, long* sum) {
    while(n--) {
        int l;
        while(!rb.pop(l));
        *sum += l;
    }
}

int main() {
    long sum = 0;
    const long n = 1024 * 1024; // * 1024;
    std::thread producer(produce, n);
    std::thread consumer(consume, n, &sum);

    producer.join();
    consumer.join();
    const long expected = n * (n-1) / 2;
    std::cout << expected << std::endl;
    std::cout << sum << std::endl;
    return !(expected == sum);
}
