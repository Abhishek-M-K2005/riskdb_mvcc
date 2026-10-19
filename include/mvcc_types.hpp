#pragma once
#include <atomic>
#include <cstdint>

// Represents a unique logical timeframe for Snapshot Isolation
using Epoch = uint64_t;
using TransactionID = uint64_t;

// A single version of a database row
struct VersionNode {
  TransactionID tx_id; // ID of the transaction that created this version
  Epoch commit_epoch;  // Timestamp when this version became visible
  std::atomic<Epoch> obsolete_epoch;
  double value; // The actual data (e.g., account balance, inventory)

  // Atomic pointer to the older version of this row (for Snapshot Isolation)
  std::atomic<VersionNode *> next_version;

  VersionNode(TransactionID id, Epoch epoch, double val)
      : tx_id(id), commit_epoch(epoch), value(val), next_version(nullptr) {}
};

// The head of the version chain for a specific database record
struct RowHeader {
  uint64_t primary_key;

  // Atomic pointer to the newest version.
  // OCC will perform a CAS on this exact pointer to commit new data.
  std::atomic<VersionNode *> latest_version;

  RowHeader(uint64_t pk) : primary_key(pk), latest_version(nullptr) {}
};