# Multi-Threaded Event Reservation System — Architecture

## 1. File structure

| File | Contents |
| --- | --- |
| `shared_table.h` / `shared_table.cpp` | The `Event` class (seat state + its own reader-writer lock), runtime configuration, the concurrency-cap primitives, and the shutdown barrier. |
| `helper.h` / `helper.cpp` | The `worker_thread` routine, per-thread statistics (`ThreadStats`), and thread-local state such as the private booking list. |
| `main.cpp` | The master thread: argument parsing and validation, event construction, worker spawn, the run timer, shutdown, statistics aggregation and the invariant check. |
| `bench.sh` | Automated harness sweeping the concurrency cap across three load regimes. |
| `Makefile` | Build (`make`) and benchmark (`make bench`). |

## 2. Core components

**Event pool** — `std::vector<std::unique_ptr<Event>>`, sized by `-e` (default 100), each
event constructed with `-c` seats (default 500). Each `Event` privately owns an
`available_seats` counter and the `pthread_rwlock_t` guarding it. `Event` is non-copyable
by design, which is why the pool holds pointers rather than values.

**Private bookings** — each worker keeps its own `std::vector<BookingRecord>` of
successful bookings, used to pick a real target for a cancellation. It is thread-local
state and needs no synchronization.

**Per-thread statistics** — attempts, successes, retries, cap-wait time, and a
pre-reserved vector of end-to-end latency samples. Each thread writes only its own
struct; `main` aggregates after `pthread_join`.

## 3. Synchronization

| Primitive | Scope | Role |
| --- | --- | --- |
| `Event::rwlock` | one per event | All data exclusion. Shared for inquiries, exclusive for bookings and cancellations. Always taken with `tryrdlock` / `trywrlock`. |
| `cap_mutex` | global | Guards `current_active_queries` **and nothing else**. It does not protect event state. |
| `active_queries_cond` | global | Parks threads while the system is at `MAX` active queries; broadcast on every slot release and at shutdown. |
| `thread_barrier` | global | Hand-rolled barrier (macOS has no `pthread_barrier_t`) where the master and all workers meet at shutdown. |

`system_running` is a `std::atomic<bool>`, set by `main` at the end of the run.

## 4. Query execution flow

1. **Generate** — pick a random query type and ticket count `k` from the thread's own
   `rand_r` stream. If the type is a cancellation, resolve the target event from the
   thread's private booking list; otherwise pick a random event. The target is resolved
   **before any lock is taken**, so the event locked is always the event mutated.
2. **Admit** — lock `cap_mutex`, wait on `active_queries_cond` while the system is at
   `MAX`, increment the counter, and unlock. `cap_mutex` is released here, before any
   event lock is taken.
3. **Execute** — call the matching `Event` method, which takes the event's rwlock in the
   appropriate mode, simulates service time while holding it, applies the change and
   unlocks.
4. **Release** — give up the cap slot and broadcast.
5. **Retry or complete** — if the event lock was contended the method returned `false`:
   count a retry, sleep 10 ms and start over with a new query. The thread **does not
   block** on the contended event. Otherwise record the latency sample and update the
   private booking list.

## 5. Lock ordering and deadlock freedom

A thread never holds `cap_mutex` and an event rwlock simultaneously — step 2 releases
`cap_mutex` before step 3 acquires an event lock. A thread also holds at most one event
rwlock at a time, because one query touches exactly one event. No thread ever holds two
locks, so no hold-and-wait cycle can form and the design is deadlock-free by construction.
The event locks are only ever taken with `try*`, so even a cycle could not block.

## 6. Shutdown

`main` sleeps for the run duration, sets `system_running` to `false`, then broadcasts
under `cap_mutex` so a thread about to park on the condvar cannot miss shutdown. Workers
observe the flag, leave their loop, and meet `main` at the barrier; `main` then joins
each thread, aggregates statistics, runs the invariant check and prints the `RESULT,`
summary line.
