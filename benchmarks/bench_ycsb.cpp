#include "lock_free_queue.hpp"
#include "mvcc_types.hpp"
#include "transaction.hpp"
#include <atomic>
#include <benchmark/benchmark.h>
#include <random>
#include <thread>
#include <vector>

// Shared Global Database for Benchmark Threads
const int NUM_ROWS = 1000;
std::vector<RowHeader *> database;
LockFreeQueue *gc_queue = nullptr;
std::atomic<Epoch> global_epoch{1};

// Initialize the database and queue exactly once before benchmarking starts
void SetupDatabase() {
  if (database.empty()) {
    for (int i = 0; i < NUM_ROWS; i++) {
      RowHeader *row = new RowHeader(i);
      row->latest_version.store(new VersionNode(0, 0, 1000.0));
      database.push_back(row);
    }
    gc_queue = new LockFreeQueue(65536);
  }
}

// The Benchmark Test: Zipfian Skew with Cooperative GC
static void BM_ZipfianWorkStealing(benchmark::State &state) {
  SetupDatabase();

  // Thread-local Random Number Generator
  std::mt19937 gen(state.thread_index() + 42);
  std::uniform_int_distribution<> hotspot_dist(0, 9);
  std::uniform_int_distribution<> cold_dist(10, NUM_ROWS - 1);
  std::uniform_int_distribution<> skew_dist(1, 100);

  // NEW: Thread-local waiting room for garbage (Epoch-Based Reclamation)
  std::vector<VersionNode *> local_deferred_garbage;

  for (auto _ : state) {
    int row_id = (skew_dist(gen) <= 90) ? hotspot_dist(gen) : cold_dist(gen);

    bool committed = false;

    // NEW: The OCC Retry Loop
    while (!committed) {
      Epoch current_epoch = global_epoch.load(std::memory_order_relaxed);
      Transaction tx(state.thread_index(), current_epoch);

      double val = tx.read(*database[row_id]);
      tx.write(*database[row_id],
               database[row_id]->latest_version.load(std::memory_order_acquire),
               val - 1.0);

      if (tx.commit(global_epoch.fetch_add(1, std::memory_order_relaxed),
                    *gc_queue)) {
        committed = true;
      } else {
        // HARDWARE FIX: Prevent Cache-Line Bouncing Livelock
        std::this_thread::yield();
      }
    }

    // NEW: Safe Epoch-Based Garbage Collection
    VersionNode *garbage = nullptr;
    if (gc_queue->pop(garbage)) {
      local_deferred_garbage.push_back(garbage);
    }

    for (auto it = local_deferred_garbage.begin();
         it != local_deferred_garbage.end();) {
      if (global_epoch.load(std::memory_order_relaxed) -
              (*it)->obsolete_epoch.load(std::memory_order_relaxed) >
          100) {
        delete *it;
        it = local_deferred_garbage.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Clean up remaining garbage for this thread
  for (auto *node : local_deferred_garbage) {
    delete node;
  }
}

BENCHMARK(BM_ZipfianWorkStealing)
    ->Threads(1)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK_MAIN();