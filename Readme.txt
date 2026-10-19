riskdb_mvcc/
├── CMakeLists.txt
├── include/
│   ├── mvcc_types.hpp       # Core data structures (Row, Version Node)
│   ├── lock_free_queue.hpp  # The MPMC work-stealing queue
│   └── transaction.hpp      # OCC logic and CAS operations
├── src/
│   ├── main.cpp             # Engine execution and thread spawning
│   └── lock_free_queue.cpp  
├── tests/
│   └── test_engine.cpp      # GTest verifications
└── benchmarks/
    └── bench_ycsb.cpp       # Google Benchmark for TPS & Tail Latency