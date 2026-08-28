# Multi-Threaded Event Reservation System

A concurrent reservation engine in C++ that simulates many worker threads issuing booking, cancellation, and inquiry queries against a shared pool of events — without a per-event lock.

## Overview

Instead of allocating a mutex per event (impractical when the number of events is large), the system enforces read/write consistency through a **shared, bounded admission table**: a fixed-size array of active-query entries, each recording an event number, a query type, and the thread that owns it. A query is only admitted if it doesn't conflict with an entry already in the table for that event. This bounds lock-table memory to `MAX` entries regardless of how many events exist, at the cost of an O(MAX) scan per admission check instead of O(1).

## Architecture & Concurrency

- **Worker threads** repeatedly generate random Inquiry / Booking / Cancellation queries and execute them concurrently.
- **Concurrency cap**: at most `MAX` queries may be active system-wide at once, enforced with a mutex + condition variable. A thread that would exceed the cap blocks on `pthread_cond_wait` until a slot frees up.
- **Read/write admission**: multiple reads on the same event may run concurrently; a write excludes all other reads and writes on that event; different events never block each other. This is enforced by scanning the shared admission table under `table_mutex` before a query is allowed to proceed.
- **Non-blocking retry**: if a query fails admission (its event is already locked by a conflicting query), the thread does _not_ block — it sleeps briefly and retries with a new random query, per the assignment's non-blocking-on-conflict requirement.
- **Custom barrier**: macOS doesn't ship `pthread_barrier_t`, so shutdown synchronization between the master thread and all workers uses a hand-rolled barrier (mutex + condition variable + counter).
- **Shutdown broadcast**: query completion signals `active_queries_cond` via `pthread_cond_broadcast`, not `pthread_cond_signal` — see [Bugs Found & Fixed](#bugs-found--fixed) for why this matters.

## Build & Run

Requires C++14 (the `QueryEntry` struct relies on aggregate initialization with default member initializers, which C++11 doesn't support).

```bash
clang++ -std=c++14 -pthread main.cpp helper.cpp shared_table.cpp -o reservation_system
```

Run with defaults (100 events, 500 capacity each, MAX=5 concurrent queries, 20 threads, 60s duration):

```bash
./reservation_system
```

Or configure at runtime:

```bash
./reservation_system -e <num_events> -c <capacity> -m <max_active_queries> -s <num_threads> -t <duration_seconds>
```

Example:

```bash
./reservation_system -e 100 -c 500 -m 10 -s 20 -t 30
```

## Bugs Found & Fixed

**1. Cancellation admission mismatch (correctness bug).**
The original implementation chose a random `event_id` for the admission-table check, but cancellations then mutated a _different_ event pulled from the thread's private booking list — meaning the locking mechanism provided zero protection for concurrent cancellations on the same event. Fixed by resolving the actual target event (including the cancellation target) _before_ requesting admission, so the event checked against the shared table is always the event actually mutated.

**2. Shutdown deadlock (`signal` vs. `broadcast`).**
Query completion originally called `pthread_cond_signal`, which wakes exactly one waiting thread. At shutdown, only as many threads as there were active queries would ever be woken — with `MAX` threads active but `NUM_THREADS - MAX` threads still parked waiting for a free slot, the remainder would never wake up, and `main()` would hang forever in `pthread_join`. Fixed by switching to `pthread_cond_broadcast`, so every waiting thread re-checks its condition (including the shutdown flag) on every completion, not just one.

**3. C++11/C++14 aggregate-initialization mismatch.**
`QueryEntry` uses default member initializers, which only qualify a struct as an aggregate (allowing brace-init assignment) starting in C++14. Compiling with `-std=c++11` failed on `shared_table[i] = {event_id, type, thread_id};`. Fixed by building with `-std=c++14`.

## Correctness Verification

At shutdown, the program checks that every event's `available_seats` stays within `[0, CAPACITY]` — a direct, automatic signal that no synchronization hole let two writers corrupt the same event concurrently, rather than relying on manual log inspection.

Per-thread stats (aggregated after `pthread_join`, so no additional locking is needed — each thread only ever writes its own `ThreadStats` struct, and `pthread_join` guarantees those writes are visible to the joining thread):

- Queries attempted / succeeded / retried (failed admission due to conflict)
- Total thread-time spent blocked on the concurrency cap

## Benchmark: Effect of `MAX` on Throughput and Contention

Run at `e=100, c=500, s=20`, varying `MAX` (`-m`) over 15-second runs:

| MAX | Queries Succeeded | Retry Rate | Cap Wait Time (thread-sec) | Throughput (queries/sec) |
| --- | ----------------- | ---------- | -------------------------- | ------------------------ |
| 3   | 1165              | 1.59%      | 210.8                      | ~78                      |
| 5   | 1960              | 3.48%      | 149.2                      | ~131                     |
| 10  | 3667              | 7.14%      | 16.2                       | ~245                     |
| 20  | 3869              | 8.81%      | 0.0                        | ~258                     |

All four runs passed the invariant check — seat counts stayed within `[0, CAPACITY]` at every concurrency level tested, including the low-MAX/high-thread-count configuration that previously exposed the shutdown deadlock.

**Finding:** raising `MAX` trades one form of contention for another. Cap-wait time falls to zero once `MAX` approaches the thread count (no thread can be blocked by the cap when there are only as many threads as available slots), but retry rate keeps climbing — with more queries in flight simultaneously, the odds of two threads targeting the same event go up, so more queries get bounced by the per-event admission check instead. Throughput gains flatten out well before `MAX` reaches the thread count: going from `MAX=10` to `MAX=20` only improves throughput by ~5% while nearly doubling the retry rate, making `MAX≈10` the efficient operating point for this workload (`e=100, s=20`) — beyond that, extra concurrency mostly produces wasted retry work rather than useful throughput.

## Parameters

| Flag | Description                   | Default |
| ---- | ----------------------------- | ------- |
| `-e` | Number of events              | 100     |
| `-c` | Seat capacity per event       | 500     |
| `-m` | Max concurrent active queries | 5       |
| `-s` | Number of worker threads      | 20      |
| `-t` | Run duration (seconds)        | 60      |
