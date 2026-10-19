#include "../include/lock_free_queue.hpp"
#include "../include/mvcc_types.hpp"
#include "../include/transaction.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <omp.h>
#include <random>
#include <vector>

using namespace std;

// System Configurations
const int NUM_ROWS = 1000;
const int NUM_TRANSACTIONS = 500000;
const int QUEUE_CAPACITY = 65536; // Must be a power of 2

// Phase 1: Workload Synthesis (Zipfian Skew Approximation)
// Simulates 90% of traffic hitting just 10 specific database rows
int get_skewed_row(mt19937 &gen) {
  uniform_int_distribution<> dist(1, 100);
  if (dist(gen) <= 90) { // The 'Flash Sale' hotspot
    uniform_int_distribution<> hot_dist(0, 9);
    return hot_dist(gen);
  } else { // The cold data
    uniform_int_distribution<> cold_dist(10, NUM_ROWS - 1);
    return cold_dist(gen);
  }
}

int main() {
  cout << "--- Starting Parallel MVCC Transaction Engine ---" << endl;

  // --- PHASE 1: System Warm-up ---
  vector<RowHeader *> database;
  for (int i = 0; i < NUM_ROWS; i++) {
    RowHeader *row = new RowHeader(i);
    row->latest_version.store(new VersionNode(0, 0, 1000.0));
    database.push_back(row);
  }

  LockFreeQueue gc_queue(QUEUE_CAPACITY);
  atomic<Epoch> global_epoch{1};

  // Telemetry tracking
  atomic<int> successful_commits{0};
  atomic<int> gc_tasks_completed{0};

  cout << "Executing Phase 2 & 3: High-Contention Workload & Cooperative GC..."
       << endl;
  auto start_time = chrono::high_resolution_clock::now();

// --- PHASE 2 & 3: Parallel Execution ---
#pragma omp parallel
  {
    random_device rd;
    mt19937 gen(rd() ^ omp_get_thread_num());

    int local_commits = 0;
    int local_gc = 0;

    // A thread-local waiting room for garbage
    std::vector<VersionNode *> local_deferred_garbage;

#pragma omp for schedule(dynamic)
    for (int i = 0; i < NUM_TRANSACTIONS; i++) {
      int row_id = get_skewed_row(gen);

      bool committed = false;

      // THE OCC RETRY LOOP
      while (!committed) {
        Epoch current_epoch = global_epoch.load(memory_order_relaxed);
        Transaction tx(i, current_epoch);

        // Read data locally
        double current_val = tx.read(*database[row_id]);

        // Perform math locally
        tx.write(*database[row_id],
                 database[row_id]->latest_version.load(memory_order_acquire),
                 current_val - 1.0);

        // Attempt CAS Commit
        if (tx.commit(global_epoch.fetch_add(1, memory_order_relaxed),
                      gc_queue)) {
          local_commits++;
          committed = true;
        }
      }

      // --- PHASE 3: Epoch-Based Garbage Collection ---
      VersionNode *garbage = nullptr;
      if (gc_queue.pop(garbage)) {
        local_deferred_garbage.push_back(garbage);
      }

      for (auto it = local_deferred_garbage.begin();
           it != local_deferred_garbage.end();) {
        if (global_epoch.load(memory_order_relaxed) -
                (*it)->obsolete_epoch.load(memory_order_relaxed) >
            100) {
          delete *it;
          local_gc++;
          it = local_deferred_garbage.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Clean up remaining nodes before the thread dies
    for (auto *node : local_deferred_garbage) {
      delete node;
      local_gc++;
    }

    successful_commits += local_commits;
    gc_tasks_completed += local_gc;
  }

  // --- PHASE 4: Quick Evaluation Snapshot ---
  auto end_time = chrono::high_resolution_clock::now();
  chrono::duration<double> elapsed = end_time - start_time;

  cout << "\n--- Engine Run Complete ---" << endl;
  cout << "Time Elapsed:          " << elapsed.count() << " seconds" << endl;
  cout << "Throughput (TPS):      " << (NUM_TRANSACTIONS / elapsed.count())
       << " req/sec" << endl;
  cout << "Goodput (Successes):   " << successful_commits.load() << " commits"
       << endl;
  cout << "Garbage Nodes Pruned:  " << gc_tasks_completed.load()
       << " nodes safely deleted" << endl;

  // Final cleanup of base database rows
  for (auto *row : database) {
    // Delete the one remaining active version (which the GC never touched)
    delete row->latest_version.load(memory_order_relaxed);
    // Delete the row header itself
    delete row;
  }
  return 0;
}