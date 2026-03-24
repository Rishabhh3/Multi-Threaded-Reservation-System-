# Multi-Threaded Event Reservation System

## Overview
[cite_start]This project is a multi-threaded event reservation system for the Nehru Centre, implemented in C/C++ using the pthread API[cite: 4, 70]. [cite_start]It simulates concurrent user queries for booking, cancelling, and checking seat availability across multiple events[cite: 71]. 

## Architecture & Concurrency
* [cite_start]Employs worker threads to execute random queries concurrently[cite: 87].
* [cite_start]Limits active concurrent queries to `MAX` to prevent system overload using condition variables [cite: 80, 97-99].
* [cite_start]Ensures data consistency for read/write operations using a custom Shared Query Table protected by mutexes[cite: 101, 110, 120].

## Compilation and Execution (macOS)
[cite_start]Since macOS does not natively support POSIX `pthread_barrier_t`, a custom barrier synchronization mechanism was implemented using a mutex and a condition variable.

**To compile:**
```bash
clang++ -std=c++11 -pthread main.cpp shared_table.cpp worker.cpp -o reservation_system

./reservation_system