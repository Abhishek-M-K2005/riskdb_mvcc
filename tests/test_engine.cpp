#include <gtest/gtest.h>
#include "mvcc_types.hpp"
#include "transaction.hpp"
#include "lock_free_queue.hpp"

class EngineConsistencyTest : public ::testing::Test {
protected:
    RowHeader* row;
    LockFreeQueue* gc_queue;
    std::atomic<Epoch> global_epoch{1};

    void SetUp() override {
        row = new RowHeader(1);
        row->latest_version.store(new VersionNode(0, 0, 100.0)); // Initial value: 100
        gc_queue = new LockFreeQueue(1024);
    }

    void TearDown() override {
        // Cleanup remaining nodes
        VersionNode* current = row->latest_version.load();
        while (current != nullptr) {
            VersionNode* next = current->next_version.load();
            delete current;
            current = next;
        }
        delete row;
        delete gc_queue;
    }
};

// Test 1: Standard isolated transaction succeeds
TEST_F(EngineConsistencyTest, SingleTransactionCommitsSuccessfully) {
    Transaction tx(1, global_epoch.load());
    double val = tx.read(*row);
    
    EXPECT_EQ(val, 100.0);
    
    tx.write(*row, row->latest_version.load(), val - 10.0);
    bool success = tx.commit(global_epoch.fetch_add(1), *gc_queue);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(row->latest_version.load()->value, 90.0);
}

// Test 2: Simulating the Flash Sale Race Condition (The Lost Update Problem)
TEST_F(EngineConsistencyTest, OCCPreventsLostUpdatesViaCAS) {
    // Both threads start at the same time and read the same initial state (100.0)
    Transaction thread_A(1, global_epoch.load());
    Transaction thread_B(2, global_epoch.load());

    double val_A = thread_A.read(*row); // Reads 100
    double val_B = thread_B.read(*row); // Reads 100

    // Both attempt to deduct 10
    thread_A.write(*row, row->latest_version.load(), val_A - 10.0);
    thread_B.write(*row, row->latest_version.load(), val_B - 10.0);

    // Thread A reaches the CPU first and commits
    bool success_A = thread_A.commit(global_epoch.fetch_add(1), *gc_queue);
    EXPECT_TRUE(success_A);
    EXPECT_EQ(row->latest_version.load()->value, 90.0);

    // Thread B tries to commit but its original pointer is now outdated. CAS MUST FAIL.
    bool success_B = thread_B.commit(global_epoch.fetch_add(1), *gc_queue);
    EXPECT_FALSE(success_B); // Consistency is saved!
    
    // The value remains 90, not 80.
    EXPECT_EQ(row->latest_version.load()->value, 90.0); 
}

// Test 3: Verifying the GC Queue receives the obsolete pointer
TEST_F(EngineConsistencyTest, SuccessfulCommitFeedsGarbageQueue) {
    Transaction tx(1, global_epoch.load());
    tx.write(*row, row->latest_version.load(), 50.0);
    tx.commit(global_epoch.fetch_add(1), *gc_queue);

    VersionNode* garbage_pointer = nullptr;
    bool has_garbage = gc_queue->pop(garbage_pointer);
    
    // The queue should now contain the original version (100.0)
    EXPECT_TRUE(has_garbage);
    EXPECT_EQ(garbage_pointer->value, 100.0);
}