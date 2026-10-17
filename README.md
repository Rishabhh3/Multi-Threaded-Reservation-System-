# Multi-Threaded Event Reservation System

A concurrent reservation engine in C++ (POSIX threads) in which each event encapsulates
its own reader-writer lock, and worker threads issue booking, cancellation and inquiry
queries against a shared pool of events under a configurable system-wide concurrency cap.

## Design

### Events own their locks

`Event` is a class, not a plain struct. It holds its seat state and the
`pthread_rwlock_t` that guards it as **private** members, and exposes only operations that
take the lock internally:

```cpp
bool try_inquire(int &seats_out);   // shared  (pthread_rwlock_tryrdlock)
bool try_book(int k, bool &booked); // exclusive (pthread_rwlock_trywrlock)
bool try_cancel(int k);             // exclusive (pthread_rwlock_trywrlock)
```

Callers cannot reach `available_seats` directly, so it is impossible to touch an event's
state without going through its lock. The copy constructor and copy assignment operator
are `= delete`d, because copying a `pthread_rwlock_t` would alias the underlying lock
state and silently break mutual exclusion. That makes `Event` non-copyable and
non-movable, so the global pool is `std::vector<std::unique_ptr<Event>>`.

This gives genuinely fine-grained read/write exclusion:

| Incoming query | Blocked by, on the same event |
| --- | --- |
| Inquiry (shared) | an in-flight booking or cancellation |
| Booking / Cancellation (exclusive) | any in-flight query, read or write |
| Any query, different event | nothing — different events are different locks |

Many inquiries on one event proceed concurrently; a writer excludes everyone on that
event; two different events never contend at all.

### Non-blocking retry

Every method uses the **try** form of the lock. On contention it returns `false`
immediately rather than blocking. The worker counts a retry, releases its cap slot, backs
off 10 ms, and comes back with a freshly generated query. Threads therefore never queue
behind a hot event — the cost of contention shows up as wasted work, which the harness
measures as the retry rate.

### Deadlock freedom

Two lock types exist: `cap_mutex` (guarding the concurrency counter) and the per-event
rwlocks. The worker releases `cap_mutex` *before* taking any event lock, so:

1. no thread ever holds `cap_mutex` and an event rwlock at the same time, and
2. each thread holds at most one event rwlock at a time — one query touches one event.

With no thread ever holding two locks, no hold-and-wait cycle can form, so the design is
deadlock-free by construction. The event locks are additionally only ever taken with
`try*`, so even a cycle could not block.

### Concurrency cap

Independently of the per-event locks, at most `MAX` queries may be active system-wide,
enforced with `cap_mutex` + `active_queries_cond`. A thread that would exceed the cap
blocks on `pthread_cond_wait` until a slot frees. This is admission control: it keeps
concurrency in the range where the machine is efficient, and it is the axis the benchmark
sweeps.

Completion broadcasts rather than signals. `pthread_cond_signal` wakes exactly one
waiter, which at shutdown leaves `NUM_THREADS - MAX` threads parked forever. Because the
shutdown barrier is sized `NUM_THREADS + 1`, a worker that never wakes never arrives at
it, so `crossing` never reaches `count` and `main` blocks inside `barrier_wait` — the join
loop on the following line is never reached. Every cap-slot release broadcasts — including
the retry path, since a slot freed without a wakeup can leave waiters parked when the
system is no longer at `MAX`.

### Simulated service time

The simulated work (`usleep`) runs **inside** the event lock. This is deliberate: the lock
would otherwise be held for a few hundred nanoseconds, contention would be effectively
zero, and the retry rate would read 0% at every `MAX` — the benchmark would measure
nothing. Holding the lock across the simulated work is what makes the read/write exclusion
observable.

### Design history

An earlier revision enforced exclusion through a single global admission table: a
fixed `MAX`-slot array of active queries, scanned under one mutex before a query was
admitted. That bounded lock memory to `MAX` entries regardless of event count, but it
serialized every admission decision through one global mutex and expressed read/write
sharing only indirectly. The per-event rwlock states the intended semantics directly and
removes the global serialization point, at the cost of `O(events)` lock memory.
`report.md` discusses that trade-off at length.

## Build & Run

Requires C++14.

```bash
make                  # g++ -std=c++14 -O2 -pthread
```

Run with defaults (100 events, 500 seats each, MAX=5, 20 threads, 60 s):

```bash
./reservation_system
```

Or configure at runtime:

```bash
./reservation_system -e 100 -c 500 -m 10 -s 20 -t 30 -q
```

All numeric arguments are validated and must be greater than 0.

### Parameters

| Flag | Description | Default |
| --- | --- | --- |
| `-e` | Number of events | 100 |
| `-c` | Seat capacity per event | 500 |
| `-m` | Max concurrent active queries | 5 |
| `-s` | Number of worker threads | 20 |
| `-t` | Run duration (seconds) | 60 |
| `-q` | Quiet: suppress per-query logging and the per-event dump | off |

## Test Harness

`bench.sh` covers the three load regimes and records throughput, latency and retry rate:

| Regime | `MAX` | Meaning |
| --- | --- | --- |
| normal | 3, 5 | cap below what the threads can drive |
| saturated | 10 | cap near the knee |
| over-subscribed | 20, 40 | cap at or above the thread count |

```bash
make bench            # or: ./bench.sh
```

Each config runs 5 repetitions of 15 seconds with `-q`. The program emits a single
machine-readable `RESULT,` line that the script parses (no grepping prose), and the script
writes `results.csv`:

```
max,run,succeeded,attempted,retry_rate,qps,lat_mean_ms,lat_p50_ms,lat_p95_ms,lat_p99_ms,cap_wait_s
```

It then prints a summary of mean ± sample standard deviation per `MAX`, and exits non-zero
if any run failed or violated the invariant.

### Metrics

- **Throughput** — queries completed per second.
- **End-to-end latency** — mean, p50, p95, p99 and max, in ms. The clock starts when a
  query is first generated and stops when one completes, so a query that is bounced and
  retried carries its original start time and is charged for the time its retries cost.
  Samples are collected per thread into a pre-reserved vector so `push_back` never
  allocates mid-measurement, then merged and sorted in `main()` after `pthread_join`.
- **Retry rate** — share of attempts turned away by a contended event lock.
- **Cap wait** — total thread-time blocked on the concurrency cap, reported separately.

## Benchmark Results

> **TODO** — fill from `bench.sh` output.

| Regime | MAX | Throughput (q/s) | Retry rate | Latency p50 (ms) | p95 (ms) | p99 (ms) | Cap wait (thread-s) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| normal | 3 | | | | | | |
| normal | 5 | | | | | | |
| saturated | 10 | | | | | | |
| over-subscribed | 20 | | | | | | |
| over-subscribed | 40 | | | | | | |

## Correctness Verification

At shutdown the program checks that every event's seat count stayed within
`[0, CAPACITY]` and prints `PASSED`/`FAILED`, exiting non-zero on failure so the harness
catches it. This is a smoke test, not a proof: a lost update could in principle leave a
count inside the range. A strictly stronger oracle is conservation — the sum of available
seats plus every thread's outstanding bookings must equal `NUM_EVENTS * CAPACITY` exactly —
which is recorded in `report.md` as the next step.

Per-thread stats are aggregated after `pthread_join`, so no locking is needed: each thread
writes only its own `ThreadStats`, and `pthread_join` makes those writes visible to the
joining thread.

## Bugs Found & Fixed

**1. Cancellation locked the wrong event (correctness).**
The original code drew a random `event_id` for the lock check, then mutated a *different*
event pulled from the thread's private booking list — so the locking protected nothing for
cancellations. Fixed by resolving the real target event before taking any lock. The thing
you lock must be the thing you mutate.

**2. Shutdown deadlock (`signal` vs `broadcast`).**
Completion signalled one waiter. At shutdown, with `MAX` threads active and
`NUM_THREADS - MAX` parked, the remainder never woke. The hang surfaced at the **barrier**,
not at the join: the barrier waits for `NUM_THREADS + 1` arrivals, a parked worker never
arrives, and `main` blocked in `barrier_wait` without ever reaching the join loop below it.
Fixed by broadcasting on every cap-slot release, so every waiter re-checks the shutdown
flag.

**3. Data race on the shutdown flag.**
`system_running` was a plain `bool` written by `main` and read by every worker with no
synchronization — a data race, and formally undefined behaviour. It is now
`std::atomic<bool>`, and `main` broadcasts under `cap_mutex` after setting it, so a thread
about to park on the condvar cannot miss shutdown and sleep through it.

**4. Shared RNG state.**
All threads called `rand()`, which shares global state and is not guaranteed thread-safe,
and it was never seeded. Replaced with `rand_r()` over a per-thread seed.

**5. Unvalidated arguments.**
`-e 0` produced `rand() % 0`. All numeric arguments are now rejected unless greater
than 0, and the parse loop advances correctly past flag values.

**6. Duplicated final output.**
`main` printed the full per-event status dump twice.

**7. Declarations outside the include guard.**
Several declarations in `shared_table.h` sat after `#endif`.
