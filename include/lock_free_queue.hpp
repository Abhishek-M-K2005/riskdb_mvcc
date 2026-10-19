#pragma once
#include "mvcc_types.hpp"
#include <atomic>
#include <cassert>
#include <vector>

// A Lock-Free Bounded Queue for Cooperative Work-Stealing
class LockFreeQueue {
private:
  struct Node {
    VersionNode *version_to_delete;
    std::atomic<bool> is_ready;

    // Add a default constructor for array initialization
    Node() : version_to_delete(nullptr), is_ready(false) {}
  };

  Node *buffer; // Replace std::vector with a raw array pointer
  const size_t capacity;
  const size_t mask;

  alignas(64) std::atomic<size_t> head;
  alignas(64) std::atomic<size_t> tail;

public:
  LockFreeQueue(size_t size)
      : capacity(size), mask(size - 1), head(0), tail(0) {
    assert((size != 0) && ((size & (size - 1)) == 0));
    // Allocate directly. The Node() constructor automatically sets is_ready to
    // false.
    buffer = new Node[capacity];
  }

  ~LockFreeQueue() {
    delete[] buffer; // Prevent memory leaks
  }
  // Active worker threads call this to dump obsolete versions
  bool push(VersionNode *version) {
    size_t current_tail = tail.load(std::memory_order_relaxed);
    size_t next_tail;

    do {
      next_tail = current_tail + 1;
      // Check if the queue is full
      if (current_tail - head.load(std::memory_order_acquire) >= capacity) {
        return false; // Queue full, GC is backing up!
      }
      // CAS: Update tail only if no other thread beat us to it
    } while (!tail.compare_exchange_weak(current_tail, next_tail,
                                         std::memory_order_release,
                                         std::memory_order_relaxed));

    // We successfully reserved a slot. Now write the data.
    size_t index = current_tail & mask; // Fast modulo
    buffer[index].version_to_delete = version;
    buffer[index].is_ready.store(true, std::memory_order_release);

    return true;
  }

  // Idle worker threads call this to steal garbage tasks
  bool pop(VersionNode *&result) {
    size_t current_head = head.load(std::memory_order_relaxed);
    size_t next_head;

    do {
      // Check if the queue is empty
      if (current_head == tail.load(std::memory_order_acquire)) {
        return false; // No work to steal
      }
      next_head = current_head + 1;
      // CAS: Update head only if no other thread stole this task first
    } while (!head.compare_exchange_weak(current_head, next_head,
                                         std::memory_order_release,
                                         std::memory_order_relaxed));

    size_t index = current_head & mask;

    // Wait for the pushing thread to finish writing the data
    while (!buffer[index].is_ready.load(std::memory_order_acquire)) {
      // Spin-wait (usually only takes a few CPU cycles)
    }

    // Safely extract the task and reset the slot
    result = buffer[index].version_to_delete;
    buffer[index].is_ready.store(false, std::memory_order_release);

    return true;
  }
};