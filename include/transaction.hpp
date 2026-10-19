#pragma once
#include "lock_free_queue.hpp"
#include "mvcc_types.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

// A private, local workspace for a transaction
struct LocalWrite {
  RowHeader *target_row;
  VersionNode *original_version_read; // Kept for CAS validation
  double new_value;
};

class Transaction {
private:
  TransactionID tx_id;
  Epoch start_epoch;
  std::vector<LocalWrite> write_set; // The private workspace

public:
  Transaction(TransactionID id, Epoch current_global_epoch)
      : tx_id(id), start_epoch(current_global_epoch) {}

  // 1. READ PHASE (Snapshot Isolation)
  double read(RowHeader &row) {
    // Start at the newest version
    VersionNode *current = row.latest_version.load(std::memory_order_acquire);

    // Traverse the lock-free linked list until we find a version committed
    // BEFORE this transaction started
    while (current != nullptr) {
      if (current->commit_epoch <= start_epoch) {
        return current->value;
      }
      current = current->next_version.load(std::memory_order_acquire);
    }
    return 0.0; // Default/null state if no valid version exists
  }

  // 2. LOCAL MATH PHASE
  void write(RowHeader &row, VersionNode *version_read, double new_value) {
    // Store the intent to write locally. Do NOT touch the shared database yet.
    write_set.push_back({&row, version_read, new_value});
  }

  // 3. VALIDATE & COMMIT PHASE
  bool commit(Epoch commit_epoch, LockFreeQueue &gc_queue) {
    std::vector<VersionNode *> successfully_committed_nodes;

    for (auto &local_write : write_set) {
      // Allocate the new version
      VersionNode *new_node =
          new VersionNode(tx_id, commit_epoch, local_write.new_value);

      // Point the new node to what we *think* is the current version
      new_node->next_version.store(local_write.original_version_read,
                                   std::memory_order_relaxed);

      // THE CAS VALIDATION:
      // "Update this row's latest pointer to my new_node, BUT ONLY IF the
      // latest pointer is still pointing to original_version_read."
      VersionNode *expected = local_write.original_version_read;
      bool success =
          local_write.target_row->latest_version.compare_exchange_strong(
              expected, new_node, std::memory_order_release,
              std::memory_order_acquire);

      if (success) {
        successfully_committed_nodes.push_back(new_node);
        if (expected != nullptr) {
          // NEW: Mark exactly when this node died
          expected->obsolete_epoch.store(commit_epoch,
                                         std::memory_order_relaxed);
          gc_queue.push(expected);
        }
      } else {
        // ABORT: Another thread updated this row before us.
        // We must rollback any nodes we already committed in this transaction.
        delete new_node;
        rollback(successfully_committed_nodes);
        return false;
      }
    }
    return true; // All writes passed CAS validation
  }

private:
  void rollback(const std::vector<VersionNode *> &committed_nodes) {
    // In a full implementation, this unlinks the nodes created during a partial
    // commit For this prototype, we rely on the GC to prune unreachable nodes
  }
};