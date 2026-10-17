# Design Report: Multi-Threaded Event Reservation System

This report explains **why** the system is built the way it is, what the alternatives were, and what each choice costs. It is meant to grow as the project does. Add a new section whenever a design decision is made.

Contents:

1. [Background: mutexes, condition variables, reader-writer locks](#1-background-mutexes-condition-variables-reader-writer-locks)
2. [Why a reader-writer lock per event?](#2-why-a-reader-writer-lock-per-event)
3. [How the per-event lock works](#3-how-the-per-event-lock-works)
4. [How concurrency is bounded](#4-how-concurrency-is-bounded)
5. [Other design decisions](#5-other-design-decisions)
6. [Measurement and test methodology](#6-measurement-and-test-methodology)
7. [Known weaknesses and open questions](#7-known-weaknesses-and-open-questions)

---

## 1. Background: mutexes, condition variables, reader-writer locks

### Mutex (mutual exclusion lock)

A mutex lets only one thread at a time run a piece of code (a *critical section*).

```
pthread_mutex_lock(&m);     // if another thread holds m, block here until it's released
  ... touch shared data ...
pthread_mutex_unlock(&m);   // let the next waiting thread in
```

It exists because `x = x + 1` is really three steps: read, add, write. Two threads can interleave those steps and lose an update. A mutex makes the steps happen as a unit.

A mutex only answers the question "may I touch this data right now?" It cannot answer "has the thing I'm waiting for happened yet?"

### Condition variable

A condition variable lets a thread **sleep until some condition becomes true**, without burning CPU in a loop. It is always paired with a mutex that protects the condition.

```
pthread_mutex_lock(&m);
while (!condition)                    // always a while loop, never an if
    pthread_cond_wait(&cv, &m);       // atomically: unlock m + sleep; re-lock m on wake
... condition is now true, and we hold m ...
pthread_mutex_unlock(&m);
```

Another thread makes the condition true and wakes sleepers:

```
pthread_mutex_lock(&m);
change_state();
pthread_cond_broadcast(&cv);          // or pthread_cond_signal to wake just one
pthread_mutex_unlock(&m);
```

Two details matter:

- **`pthread_cond_wait` unlocks the mutex and goes to sleep as one atomic step.** Without that, a thread could unlock, then the state could change and the wakeup could fire, and only then would the thread go to sleep. It would sleep forever (a *lost wakeup*).
- **The `while` loop is mandatory.** Threads can wake spuriously. Also, when many threads are woken, another thread may take the resource first, so the condition must be re-checked after every wakeup.

### Reader-writer lock

A reader-writer lock is **one** lock object that a thread takes in one of two modes:

```
pthread_rwlock_rdlock(&rw);   // shared:    many threads may hold it at once
pthread_rwlock_wrlock(&rw);   // exclusive: one thread, and no readers
pthread_rwlock_unlock(&rw);   // releases either mode
```

It is not two separate locks. The rule is *many readers **or** one writer*. It exists because a plain mutex is too strong for read-mostly data: two threads that only look at a value cannot corrupt it, so forcing them to take turns throws away concurrency for nothing.

The cost is size and complexity. On 64-bit macOS a `pthread_mutex_t` is 64 bytes, while a `pthread_rwlock_t` is 200 — it has to track a reader count, not just an owner.

### The `try` variants

Every POSIX lock has a non-blocking form:

```
if (pthread_rwlock_trywrlock(&rw) != 0) {
    // contended RIGHT NOW -- did not block, did not acquire
}
```

It either acquires the lock immediately or returns an error immediately. It never waits. This is the primitive that makes "on conflict, go do something else" expressible, and it is what the whole retry path in this system is built on.

### Used together here

| Primitive | Role in this project |
| --- | --- |
| `Event::rwlock` | One per event. All data exclusion. Shared for inquiries, exclusive for bookings and cancellations. Always taken with `try*`. |
| `cap_mutex` | Protects `current_active_queries` and nothing else. Held only for a counter update. |
| `active_queries_cond` | Lets threads sleep while the system is at `MAX` active queries. Woken when any query finishes, and at shutdown. |

---

## 2. Why a reader-writer lock per event?

The system gives every `Event` its own `pthread_rwlock_t`, held privately inside the class.

### What it replaced

An earlier revision of this project did something different: a single global **admission table**, a fixed array of `MAX` slots recording "thread T is running query type Q on event E". A query was admitted only if scanning the table under one global mutex found no conflicting entry for that event. A slot's presence *was* the lock.

That design had one strong argument in its favour, and it is worth stating honestly because it is the cost the current design pays:

> **Lock memory is bounded by concurrency, not by data size.** The table needs `MAX` entries no matter how many events exist. At 100 million events, a `pthread_rwlock_t` each would be about 20 GB of lock state, almost all of it for events nobody is touching. The table needs a handful of slots.

So why move away from it?

1. **It serialized every admission decision.** Every thread, for every query, had to take the *same* global `table_mutex` to run its check-and-register step. Queries ran in parallel afterwards, but the decision to run was a single-threaded bottleneck, and the scan inside it was `O(MAX)`. With per-event locks there is no global lock on the data path at all: two threads touching two different events share nothing, not even a cache line.
2. **It expressed read/write sharing indirectly.** The "many readers or one writer" rule lived in the branching inside `can_admit_query`, a hand-written scan that had to be read carefully to be believed. A reader-writer lock *is* that rule, implemented and tested by the platform. Fewer lines of our own code stand between the intent and the behaviour.
3. **Correctness depended on a non-local invariant.** `add_query_to_table` could in principle return `-1` (no free slot), and the caller never checked it; the reason it never happened was an argument about the cap living in a different function. Encapsulating state behind `Event`'s methods removes that class of reasoning: the lock and the data it guards are the same object.
4. **It coupled two unrelated knobs.** `MAX` is admission control — a statement about how much concurrency the machine should allow. Table size is a data-structure dimension. In the old design they had to be the same number. They are now independent.

**The honest trade-off.** We bought directness and removed a global serialization point, and we paid for it in memory that scales with the number of events. At 100 events, 200 bytes each is 20 KB and the question does not arise. At 100 million events this design is simply wrong, and §7 records what to do instead.

Note that the old report argued *against* per-event locking partly on the grounds that a blocking mutex would be held across the slow part of a query. That objection is answered by `trywrlock`, not by abandoning per-event locks: the lock *is* held across the simulated work (§5.3), but a thread that cannot get it does not wait.

### Alternatives considered

| Alternative | Idea | Why not chosen |
| --- | --- | --- |
| Global admission table | `MAX` slots, scanned under one mutex | Global serialization point; read/write rule hand-written; see above |
| Per-event plain `pthread_mutex_t` | One mutex per event | Cannot express reader sharing — concurrent inquiries on one event would take turns for no reason |
| One global mutex around everything | Hold it for the whole query | Correct and trivial, but zero concurrency: the simulated work would be fully serialized |
| Striped / hashed locks | `N` locks (say 64); event *i* uses lock `i % N` | Bounded memory *and* no global bottleneck — the right answer at very large event counts. Costs false conflicts between unrelated events that hash together. The natural next step if the catalog grows |
| Atomics / lock-free on the seat counter | `fetch_sub` with a CAS retry | Works for a plain booking, but there is no shared-vs-exclusive distinction to express, and a multi-step invariant is harder to keep consistent |

---

## 3. How the per-event lock works

`Event` holds its state and its lock as private members:

```cpp
class Event {
public:
    bool try_inquire(int &seats_out);    // tryrdlock
    bool try_book(int k, bool &booked);  // trywrlock
    bool try_cancel(int k);              // trywrlock
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
private:
    int id;
    int available_seats;
    pthread_rwlock_t rwlock;
};
```

Three things are doing work here.

**Encapsulation is the enforcement.** `available_seats` is private, and no method hands out a reference to it. There is no way to reach an event's state except through a method that takes the lock first. In the old design, taking the lock and touching the data were two separate steps a caller could get wrong — and did get wrong, in exactly the way §5.1 describes. Here the compiler makes that mistake unavailable.

**Copying is deleted, and that is not a style choice.** A `pthread_rwlock_t` is an opaque blob of lock state. Copying an `Event` would bit-copy that blob, producing two objects that each think they own a lock while the kernel knows about one. Mutual exclusion would silently stop working, with no error anywhere. Deleting the copy operations turns a silent correctness disaster into a compile error.

The consequence is that `Event` is neither copyable nor movable, so `std::vector<Event>` will not compile — a vector must be able to relocate its elements when it grows. The pool is therefore `std::vector<std::unique_ptr<Event>>`: the pointers move, the events never do.

**Exclusion rules.** These are the rwlock's own semantics, not something this code implements:

| Incoming query | Blocked by, on the same event |
| --- | --- |
| Inquiry (shared) | an in-flight booking or cancellation |
| Booking / Cancellation (exclusive) | any in-flight query, read or write |
| Any query, different event | nothing — different events are different locks |

Many inquiries on one event proceed together. A writer excludes everyone on that event. Two different events never interact, because they are two different lock objects with no shared state.

**Nothing blocks.** Every method uses the `try` form, so contention is reported, not waited on:

```cpp
bool Event::try_book(int k, bool &booked) {
    if (pthread_rwlock_trywrlock(&rwlock) != 0)
        return false;             // contended -- caller's problem now
    simulate_service_time();      // held across the work: see §5.3
    if (available_seats >= k) { available_seats -= k; booked = true; }
    else                      { booked = false; }
    pthread_rwlock_unlock(&rwlock);
    return true;
}
```

Returning `false` is what drives the retry path in §4. Note the distinction the signature draws: the **return value** says whether the lock was obtained, and `booked` says whether the booking succeeded on its merits. A full event and a contended event are different outcomes and are counted separately.

---

## 4. How concurrency is bounded

Two mechanisms operate, and they are easy to confuse. One is about the *machine*, the other about the *data*.

### Mechanism 1: the global cap (condition variable)

```cpp
pthread_mutex_lock(&cap_mutex);
while (current_active_queries >= MAX_ACTIVE_QUERIES && system_running)
    pthread_cond_wait(&active_queries_cond, &cap_mutex);
current_active_queries++;
pthread_mutex_unlock(&cap_mutex);
```

If `MAX` queries are already running, the thread **sleeps** here. This is admission control: hardware is finite, and past some concurrency level the machine spends its time context-switching rather than working. It has nothing to do with which event is being touched — a query waiting here might not conflict with anybody.

### Mechanism 2: the per-event lock (trylock)

```cpp
acquired = events[event_id]->try_book(k, booked);
release_cap_slot();
if (!acquired) {
    stats.queries_retried++;
    usleep(10000);     // back off briefly...
    continue;          // ...and come back with a NEW random query
}
```

If another thread holds that event's lock in a conflicting mode, this thread is **turned away without blocking**. It gives its cap slot back, sleeps 10 ms, and generates a different query.

This is deliberately not a queue. The thread does not wait for *that* event; it abandons the work and does something else. The benefit is that threads never pile up behind one hot event. The cost is wasted work, which is exactly what the retry-rate metric measures (§6).

### Why the cap slot is released before the retry

A thread that fails to get an event lock still holds a cap slot. It must give that back before sleeping, or the cap would be occupied by a thread doing nothing. More subtly, **every** release must broadcast, including this one:

```cpp
static void release_cap_slot() {
    pthread_mutex_lock(&cap_mutex);
    current_active_queries--;
    pthread_cond_broadcast(&active_queries_cond);
    pthread_mutex_unlock(&cap_mutex);
}
```

A slot freed without a wakeup can leave threads parked on the condvar even though the system is no longer at `MAX`.

### Deadlock freedom

There are two kinds of lock in the system, so the question is real. The argument is structural rather than empirical:

1. `cap_mutex` is released *before* any event lock is taken. No thread ever holds `cap_mutex` and an event rwlock at the same time.
2. A thread holds at most one event rwlock at a time — one query touches exactly one event.

No thread ever holds two locks, so no hold-and-wait cycle can form, and deadlock is impossible by construction rather than by convention. There is no lock *ordering* to document and no ordering discipline for a future change to violate.

As a second line of defence, the event locks are only ever taken with `try*`, which cannot block. Even if a future change did create a cycle, it would surface as livelock or a rising retry rate — visible in the metrics — rather than a silent hang.

### Summary

- The **rwlock** stays held for the whole query and is what protects the data.
- The **condition variable** parks threads when the machine is at capacity.
- The **retry-with-backoff** handles per-event conflicts without blocking.
- The two mechanisms are independent: the cap is about the machine, the lock is about the data.

---

## 5. Other design decisions

### 5.1 Resolve the target event *before* taking any lock

A cancellation acts on an event drawn from the thread's private booking list. The original code checked admission on a *random* event but then cancelled a *different* one, so the locking protected nothing. The target is now resolved first, and the lock taken is the lock on that event.

*Principle:* the thing you lock must be the thing you mutate. The current encapsulation makes this much harder to get wrong, since the lock is reached only through the object that owns the data — but the ordering still matters, because the thread must know *which* object before it can call a method on it.

### 5.2 `pthread_cond_broadcast` instead of `pthread_cond_signal`

`signal` wakes one waiter. At shutdown, if `MAX` threads were active and `NUM_THREADS − MAX` were parked, only `MAX` completions would wake `MAX` threads, and the rest would sleep forever.

**Where the hang actually appeared matters.** It is tempting to say `pthread_join` hung, but `main` never got that far. The shutdown barrier is initialized to `NUM_THREADS + 1`, so it does not release until every worker *and* `main` have arrived. A worker still parked on the condition variable never arrives, `crossing` never reaches `count`, and `main` blocks inside `barrier_wait` — the `pthread_join` loop on the next line is unreachable. Anyone reading `main.cpp` sees `barrier_wait` before the joins, so a description that blames the join is describing a different program.

`broadcast` wakes every waiter, and each re-checks the condition and the shutdown flag. The cost is a "thundering herd": all waiters wake although only one slot freed. That is acceptable at 20 threads, and correctness matters more here.

*Alternatives:* a semaphore for the cap would be a natural fit, but shutdown would still have to wake everyone. Two condition variables — one for "slot free", one for "shutdown" — would avoid the herd, at the cost of more code for little gain at this scale.

### 5.3 The simulated service time runs *inside* the event lock

This is the least obvious decision in the system and the easiest to get wrong.

The simulated work is a `usleep` of 10–60 ms, and it happens between acquiring and releasing the event's rwlock. The alternative — take the lock, mutate, release, *then* sleep — looks more conventional and is wrong for this project:

> The lock would be held for a few hundred nanoseconds. With 20 threads over 100 events, the chance that two threads hold the same event lock in that window is effectively zero. The retry rate would read 0.00% at every value of `MAX`, the read/write exclusion would never be exercised even once, and the benchmark would be measuring nothing.

Holding the lock across the simulated work is what makes the exclusion *observable*. It models a query whose service time is real work against the event — which is what a reservation query actually is.

The implementation consequence is awkward and worth recording: the sleep needs the thread's RNG state, but the method signatures take no seed. It is routed through a `thread_local unsigned int rng_seed` that each worker initializes once. A `thread_local` is the right tool here precisely because the state is per-thread by nature and passing it through every signature would be noise.

### 5.4 Non-blocking retry with a *new* random query

On conflict the thread backs off and generates a different query rather than waiting for the busy event. This avoids threads queueing behind a hot event. The cost is wasted work, measured as the retry rate.

It also means the system makes **no fairness guarantee**. There is no queue, so there is no FIFO order and no bound on how long a particular query might keep losing. §7 records the starvation case this admits.

### 5.5 A hand-written barrier

macOS does not provide `pthread_barrier_t`, so `MacBarrier` is built from a mutex, a condition variable and a counter. The master and all workers meet at it during shutdown.

*Alternatives:* `pthread_join` alone would be sufficient for shutdown; the barrier is there because the assignment asks for one. C++20's `std::barrier` would be the modern answer. The current implementation has a real defect, recorded in §7.

### 5.6 Per-thread stats, aggregated after `pthread_join`

Each thread writes only its own `ThreadStats`, so no locking is needed, and `pthread_join` guarantees those writes are visible to `main`. The alternative — shared atomic counters — would cost cache-line contention on every update for no benefit, and would perturb the very timings being measured.

The latency sample vector is `reserve`d up front for the same reason: a `push_back` that reallocates in the middle of a measured region would charge an allocation to a latency sample.

### 5.7 `std::atomic<bool>` for the shutdown flag

`system_running` was a plain `bool` written by `main` and read by every worker with no synchronization. That is a data race, and formally undefined behaviour: nothing stops a compiler from hoisting the load out of the loop, since in a race-free program nobody else could have changed it.

It is now `std::atomic<bool>`, loaded with `memory_order_relaxed` — no ordering is needed, only atomicity and a guarantee that the value is actually re-read. `main` additionally broadcasts under `cap_mutex` *after* setting it, closing the window where a thread checks the flag, decides to park, and reaches `pthread_cond_wait` after the only wakeup has already fired.

### 5.8 Per-thread `rand_r`

All threads previously called `rand()`, which shares global state across threads, is not required by POSIX to be thread-safe, and was never seeded — so every run was identical. Each thread now seeds its own `rand_r` state. §7 records a remaining weakness in how that seed is derived.

### 5.9 Runtime configuration, validated

`NUM_EVENTS`, `CAPACITY`, `MAX`, thread count and duration are flags, so the harness can sweep `MAX` without recompiling. Values are rejected unless greater than zero: `-e 0` previously reached `rand() % 0`, which is undefined behaviour and in practice a crash.

A `-q` flag suppresses per-query logging. This is not cosmetic — the harness must run quiet, because 20 threads writing to `std::cout` both interleave their output and add I/O time to the measurements.

### 5.10 A machine-readable `RESULT,` line

The program prints one line of `key=value` pairs covering every metric, which `bench.sh` parses. The alternative is grepping the human-readable prose, which breaks the moment anyone rewords a message. The two formats have different audiences and different stability requirements, so they are produced separately.

---

## 6. Measurement and test methodology

### What is measured

| Metric | Definition | Why it is interesting |
| --- | --- | --- |
| Throughput | queries completed / second | The headline number, but see the caveat below |
| Latency | mean, p50, p95, p99, max, in ms | Tail latency is where cap queueing shows up; the mean hides it |
| Retry rate | retries / attempts | The cost of per-event contention — wasted work |
| Cap wait | total thread-time blocked on the condvar | The cost of admission control, kept separate from retries |

Retries and cap waits are deliberately **not** merged. They are two different kinds of contention with opposite responses to `MAX`: raising the cap removes cap waiting and creates retries. Reporting one number would hide the trade-off that the whole benchmark exists to show.

### How latency is defined

The clock starts when a query is first generated and stops when one completes. A query that is bounced and retried **keeps its original start time**, so the 10 ms backoff and the wasted attempt are charged to that sample. This is on purpose: it measures what a client would experience, not what the system would like to take credit for.

Samples are collected per thread into a pre-reserved vector, then merged and sorted in `main()` after `pthread_join`, and percentiles are taken by nearest rank.

### The three regimes

`bench.sh` sweeps `MAX` at fixed `e=100 c=500 s=20`, five repetitions of 15 seconds each:

| Regime | `MAX` | What it probes |
| --- | --- | --- |
| normal | 3, 5 | Cap below what the threads can drive — throughput is cap-limited, latency is dominated by queueing |
| saturated | 10 | Near the knee |
| over-subscribed | 20, 40 | Cap at or above the thread count — the cap stops binding entirely, so `MAX=40` should look identical to `MAX=20` |

The over-subscribed case is a control as much as a measurement: if `MAX=40` differed meaningfully from `MAX=20` with only 20 threads, something would be wrong with the harness, because no more than 20 queries can be in flight regardless of the cap.

Every run is checked for the invariant, and the script exits non-zero if any run fails, so the harness is a test and not only a benchmark.

### The throughput number is sleep-bound, and this matters

The simulated service is `usleep`, which consumes no CPU. Mean service time is 35 ms and the mean post-query sleep is another 35 ms, so a simple queueing argument predicts:

```
throughput ≈ min( MAX / 0.035 ,  NUM_THREADS / 0.070 )
```

Measured throughput tracks that model to roughly 90% across the whole sweep, with the gap accounted for by retries and backoff. That is a useful sanity check on the harness — but it also means **these numbers measure the sleep, not the lock**. The shape of the `MAX` curve is real; the absolute queries-per-second figure would change entirely with real CPU-bound query work, and the efficient `MAX` would move toward the core count. Any claim this project makes about throughput has to carry that caveat.

### What the invariant check does and does not prove

At shutdown, every event's seat count is checked against `[0, CAPACITY]`. If two writers ever corrupted one event concurrently, a lost update would *likely* push a count outside that range. It is automatic and repeatable, unlike reading logs.

It is **not a proof**. A lost update that happens to leave the count inside the range passes silently. §7 records the stronger oracle.

---

## 7. Known weaknesses and open questions

Real issues, kept here so the reasoning behind a future fix is recorded.

### 7.1 The invariant check is a weak oracle

Range-checking catches gross corruption and misses a lost update that stays in range. A strictly stronger and equally cheap oracle is **conservation**:

```
sum(available_seats over all events)
  + sum(outstanding private_bookings over all threads)
  == NUM_EVENTS * CAPACITY        // exactly, always
```

Every booking moves `k` from the first term to the second; every cancellation moves it back. A lost update breaks the equality exactly, with no false negatives and no false positives. It needs each worker's `private_bookings` to survive to the join, which is a small change. **This is the single highest-value improvement outstanding.**

### 7.2 The barrier is not spurious-wakeup safe

`barrier_wait` calls `pthread_cond_wait` inside an `if`, not a `while`, and has no generation counter:

```cpp
if (barrier->crossing >= barrier->count) { barrier->crossing = 0; broadcast(); }
else                                     { pthread_cond_wait(...); }   // not a loop
```

A spurious wakeup lets a thread leave the barrier before the last one arrives, and resetting `crossing` to zero before the waiters have woken means the barrier cannot safely be reused. It is benign today only because it is used exactly once, at shutdown, and `pthread_join` provides the real synchronization afterwards.

This directly contradicts the rule stated in §1 — the `while` loop is mandatory — and is the clearest outstanding defect in the code. *Fix:* a generation counter and a `while` loop, or C++20 `std::barrier`.

### 7.3 `queries_succeeded` does not mean what it says

It counts queries that acquired the lock and executed. A booking rejected for insufficient seats still increments it. The name promises a business outcome and the counter delivers a concurrency outcome.

It is left as-is so the throughput and retry figures stay comparable with previously published runs, but the honest fix is to rename it `queries_executed` and add a separate `bookings_rejected`. A metric whose name does not match its definition is a defect even when the number is correct.

### 7.4 Retry accounting is self-referential

`queries_attempted` is incremented on every loop iteration, retries included, so the retry rate is retries divided by attempts-*including*-retries. That is a defensible definition but not the only one, and it is not stated anywhere a reader of the CSV would see it.

### 7.5 The RNG seeds are correlated

Each thread seeds from `time(NULL) ^ thread_id`. For ids 0–19 the seeds differ only in the low bits, and `rand_r` is a weak linear congruential generator whose low bits have short periods. The streams are therefore not as independent as "each thread has its own RNG" implies. *Fix:* hash the id first, e.g. `time(NULL) ^ (id * 2654435761u)`, or use `<random>` with a properly seeded per-thread engine.

### 7.6 Writer starvation is possible in principle

A booking on a hot event that is under continuous read load will keep failing `trywrlock`. Because the thread abandons the query and generates a new one rather than queueing, no *particular* write starves forever — but writes to a persistently hot event could be rare, and nothing in the design bounds that. The system trades fairness for non-blocking progress, deliberately, and currently does not measure the fairness it gave up. A per-event retry histogram would show whether this happens in practice.

Relatedly, `pthread_rwlock_tryrdlock` is permitted to fail when a writer is *waiting*, not only when one holds the lock. Since this code never blocks on these locks, that mostly does not arise, but it means an occasional retry may not correspond to a true conflict.

### 7.7 Lock memory scales with the event count

200 bytes per event is 20 KB at the current scale and 20 GB at 100 million events. This design does not scale to very large catalogs. The intended answer at that size is striped locks (§2) or a sharded concurrent map keyed by event id, accepting false conflicts in exchange for bounded memory.

### 7.8 There are no unit tests

The exclusion rules are now the platform's rather than ours, but `Event`'s *use* of them is still untested: that `try_inquire` admits a second concurrent reader, that `try_book` excludes one, that a failed `try_*` leaves the seat count untouched. These are deterministic two-thread tests that need no timing luck, and none exist. Everything currently rests on one end-of-run invariant check.

### 7.9 No race detector has been run

Sanitizers are not usable on the development machine (Apple Silicon macOS). ThreadSanitizer crashes immediately on `int main(){return 0;}` — reproducible, instant, exit 139. An AddressSanitizer/UBSan build of that same trivial program behaves differently and worse: it does not crash, it spins. It ran for over two minutes without finishing, pegging a core hard enough to heat the machine noticeably, and had to be killed by hand.

The uninstrumented binary runs fine, so this is a toolchain limitation rather than anything wrong with this program — but the practical effect is that **no instrumented verification has been performed at all**, not merely no race detection. It is also a reason not to keep retrying locally: the failure mode burns CPU indefinitely rather than returning an error.

So every concurrency claim in this report — §4's deadlock-freedom argument, §5.7 on the shutdown flag, §5.8 on RNG state — is **reasoned, not measured**. Verifying them needs a Linux container or CI runner with TSan, plus Helgrind as a second opinion; both are cheap to add and neither exists yet. Until then the distinction between *verified* and *reasoned* should be stated rather than blurred, because an argument that sounds airtight is exactly the kind that hides a bug.

### 7.10 Output is still unsynchronized

`std::cout` from 20 threads can interleave mid-line. `-q` keeps this out of the benchmarks entirely, so it no longer affects any measurement, but verbose output remains unreliable for diagnosis — which is unfortunate, since diagnosis is when you want it most.

### 7.11 The benchmark table in the README is unfilled

`bench.sh` exists and is tested, but the published results table is still a placeholder. The numbers in §6 come from short ad-hoc runs, not from the five-repetition harness.
