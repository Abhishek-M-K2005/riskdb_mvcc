#include "lock_free_queue.hpp"
#include "mvcc_types.hpp"
#include "transaction.hpp"
#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

const int NUM_ROWS = 1000;
const int GC_CAPACITY = 65536;

// Helper to create an isolated database
std::vector<RowHeader *> CreateDatabase() {
  std::vector<RowHeader *> db;
  db.reserve(NUM_ROWS);
  for (int i = 0; i < NUM_ROWS; i++) {
    RowHeader *row = new RowHeader(i);
    row->latest_version.store(new VersionNode(0, 0, 1000.0));
    db.push_back(row);
  }
  return db;
}

// ============================================================================
// 1. ISOLATED DATABASE: Traditional Pessimistic 2PL Mutex
// ============================================================================
std::vector<RowHeader *> mutex_db;
LockFreeQueue *mutex_gc_queue = nullptr;
std::mutex row_locks[NUM_ROWS];
std::atomic<Epoch> mutex_epoch{1};

void SetupMutex() {
  if (mutex_db.empty()) {
    mutex_db = CreateDatabase();
    mutex_gc_queue = new LockFreeQueue(GC_CAPACITY);
  }
}

static void BM_Traditional_Mutex(benchmark::State &state) {
  SetupMutex();
  std::mt19937 gen(state.thread_index() + 42);
  std::uniform_int_distribution<> hotspot_dist(0, 9);
  std::uniform_int_distribution<> cold_dist(10, NUM_ROWS - 1);
  std::uniform_int_distribution<> skew_dist(1, 100);

  for (auto _ : state) {
    int row_id = (skew_dist(gen) <= 90) ? hotspot_dist(gen) : cold_dist(gen);

    std::lock_guard<std::mutex> lock(row_locks[row_id]);

    Epoch current_epoch = mutex_epoch.load(std::memory_order_relaxed);
    Transaction tx(state.thread_index(), current_epoch);
    double val = tx.read(*mutex_db[row_id]);
    tx.write(*mutex_db[row_id],
             mutex_db[row_id]->latest_version.load(std::memory_order_acquire),
             val - 1.0);
    tx.commit(mutex_epoch.fetch_add(1, std::memory_order_relaxed),
              *mutex_gc_queue);

    VersionNode *garbage = nullptr;
    if (mutex_gc_queue->pop(garbage)) {
      delete garbage;
    }
  }
}

// ============================================================================
// 2. ISOLATED DATABASE: RiskDB OCC Engine (Pure Working Design from Notes)
// ============================================================================
std::vector<RowHeader *> occ_db;
LockFreeQueue *occ_gc_queue = nullptr;
std::atomic<Epoch> occ_epoch{1};

void SetupOCC() {
  if (occ_db.empty()) {
    occ_db = CreateDatabase();
    occ_gc_queue = new LockFreeQueue(GC_CAPACITY);
  }
}

static void BM_ZipfianWorkStealing(benchmark::State &state) {
  SetupOCC();
  std::mt19937 gen(state.thread_index() + 42);
  std::uniform_int_distribution<> hotspot_dist(0, 9);
  std::uniform_int_distribution<> cold_dist(10, NUM_ROWS - 1);
  std::uniform_int_distribution<> skew_dist(1, 100);
  std::vector<VersionNode *> local_deferred_garbage;

  for (auto _ : state) {
    int row_id = (skew_dist(gen) <= 90) ? hotspot_dist(gen) : cold_dist(gen);
    bool committed = false;

    // THE PROVEN OCC RETRY LOOP (From your original main.cpp & PDF notes)
    while (!committed) {
      Epoch current_epoch = occ_epoch.load(std::memory_order_relaxed);
      Transaction tx(state.thread_index(), current_epoch);

      double val = tx.read(*occ_db[row_id]);
      tx.write(*occ_db[row_id],
               occ_db[row_id]->latest_version.load(std::memory_order_acquire),
               val - 1.0);

      if (tx.commit(occ_epoch.fetch_add(1, std::memory_order_relaxed),
                    *occ_gc_queue)) {
        committed = true;
      } else {
        // Hardware Fix: Prevent Cache-Line Bouncing Livelock
        std::this_thread::yield();
      }
    }

    // Safe Epoch-Based Garbage Collection (100-epoch grace)
    VersionNode *garbage = nullptr;
    if (occ_gc_queue->pop(garbage)) {
      local_deferred_garbage.push_back(garbage);
    }
    for (auto it = local_deferred_garbage.begin();
         it != local_deferred_garbage.end();) {
      if (occ_epoch.load(std::memory_order_relaxed) -
              (*it)->obsolete_epoch.load(std::memory_order_relaxed) >
          100) {
        delete *it;
        it = local_deferred_garbage.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto *node : local_deferred_garbage)
    delete node;
}

// ============================================================================
// 3. ISOLATED DATABASE: CMU Cicada (Early Abort via Pointer Check + Adaptive Yield)
// ============================================================================
std::vector<RowHeader *> cicada_db;
LockFreeQueue *cicada_gc_queue = nullptr;
std::atomic<Epoch> cicada_epoch{1};

void SetupCicada() {
  if (cicada_db.empty()) {
    cicada_db = CreateDatabase();
    cicada_gc_queue = new LockFreeQueue(GC_CAPACITY);
  }
}

static void BM_Cicada_Full_Architecture(benchmark::State &state) {
  SetupCicada();
  std::mt19937 gen(state.thread_index() + 42);
  std::uniform_int_distribution<> hotspot_dist(0, 9);
  std::uniform_int_distribution<> cold_dist(10, NUM_ROWS - 1);
  std::uniform_int_distribution<> skew_dist(1, 100);
  std::vector<VersionNode *> thread_local_gc;

  for (auto _ : state) {
    int row_id = (skew_dist(gen) <= 90) ? hotspot_dist(gen) : cold_dist(gen);
    bool committed = false;
    int contention_failures = 0;

    while (!committed) {
      Epoch current_epoch = cicada_epoch.load(std::memory_order_relaxed);
      VersionNode *latest =
          cicada_db[row_id]->latest_version.load(std::memory_order_acquire);

      Transaction tx(state.thread_index(), current_epoch);
      double val = tx.read(*cicada_db[row_id]);

      // Safe Cicada Pointer Check (Early Abort if another thread changed the head)
      VersionNode *head_now =
          cicada_db[row_id]->latest_version.load(std::memory_order_relaxed);
      if (head_now != latest) {
        contention_failures++;
        if (contention_failures % 2 == 0) {
          std::this_thread::yield();
        }
        continue;
      }

      tx.write(*cicada_db[row_id], latest, val - 1.0);

      if (tx.commit(cicada_epoch.fetch_add(1, std::memory_order_relaxed),
                    *cicada_gc_queue)) {
        committed = true;
      } else {
        contention_failures++;
        std::this_thread::yield();
      }
    }

    VersionNode *garbage = nullptr;
    if (cicada_gc_queue->pop(garbage))
      thread_local_gc.push_back(garbage);
    for (auto it = thread_local_gc.begin(); it != thread_local_gc.end();) {
      if (cicada_epoch.load(std::memory_order_relaxed) -
              (*it)->obsolete_epoch.load(std::memory_order_relaxed) >
          100) {
        delete *it;
        it = thread_local_gc.erase(it);
      } else
        ++it;
    }
  }
  for (auto *node : thread_local_gc)
    delete node;
}

// ============================================================================
// 4. ISOLATED DATABASE: Microsoft Hekaton (Validation Phase Simulation)
// ============================================================================
std::vector<RowHeader *> hekaton_db;
LockFreeQueue *hekaton_gc_queue = nullptr;
std::atomic<Epoch> hekaton_epoch{1};

void SetupHekaton() {
  if (hekaton_db.empty()) {
    hekaton_db = CreateDatabase();
    hekaton_gc_queue = new LockFreeQueue(GC_CAPACITY);
  }
}

static void BM_Hekaton_True_Validation(benchmark::State &state) {
  SetupHekaton();
  std::mt19937 gen(state.thread_index() + 42);
  std::uniform_int_distribution<> hotspot_dist(0, 9);
  std::uniform_int_distribution<> cold_dist(10, NUM_ROWS - 1);
  std::uniform_int_distribution<> skew_dist(1, 100);
  std::vector<VersionNode *> local_deferred_garbage;

  for (auto _ : state) {
    int row_id = (skew_dist(gen) <= 90) ? hotspot_dist(gen) : cold_dist(gen);
    bool committed = false;

    while (!committed) {
      Epoch current_epoch = hekaton_epoch.load(std::memory_order_relaxed);
      Transaction tx(state.thread_index(), current_epoch);

      VersionNode *read_version =
          hekaton_db[row_id]->latest_version.load(std::memory_order_acquire);
      double val = tx.read(*hekaton_db[row_id]);

      bool validation_passed = (hekaton_db[row_id]->latest_version.load(
                                    std::memory_order_acquire) == read_version);

      if (validation_passed) {
        tx.write(*hekaton_db[row_id], read_version, val - 1.0);
        if (tx.commit(hekaton_epoch.fetch_add(1, std::memory_order_relaxed),
                      *hekaton_gc_queue)) {
          committed = true;
        } else {
          std::this_thread::yield();
        }
      } else {
        std::this_thread::yield();
      }
    }

    VersionNode *garbage = nullptr;
    if (hekaton_gc_queue->pop(garbage)) {
      local_deferred_garbage.push_back(garbage);
    }
    for (auto it = local_deferred_garbage.begin();
         it != local_deferred_garbage.end();) {
      if (hekaton_epoch.load(std::memory_order_relaxed) -
              (*it)->obsolete_epoch.load(std::memory_order_relaxed) >
          100) {
        delete *it;
        it = local_deferred_garbage.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto *node : local_deferred_garbage)
    delete node;
}

// ============================================================================
// BENCHMARK REGISTRATIONS
// ============================================================================
BENCHMARK(BM_Traditional_Mutex)
    ->Threads(1)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK(BM_ZipfianWorkStealing)
    ->Threads(1)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK(BM_Cicada_Full_Architecture)
    ->Threads(1)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK(BM_Hekaton_True_Validation)
    ->Threads(1)
    ->Threads(4)
    ->Threads(8)
    ->UseRealTime();

BENCHMARK_MAIN();
