#ifndef SHARED_TABLE_H
#define SHARED_TABLE_H

#include <atomic>
#include <memory>
#include <vector>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Runtime configuration. Written once in main() from argv, read-only after the
// worker threads are spawned, so no synchronization is needed on these.
// ---------------------------------------------------------------------------
extern int NUM_EVENTS;
extern int CAPACITY;
extern int MAX_ACTIVE_QUERIES;   // system-wide cap on concurrently active queries
extern int NUM_THREADS;
extern int RUN_DURATION_SEC;
extern bool quiet_mode;          // -q: suppress per-query logging

enum QueryType { INQUIRY, BOOKING, CANCELLATION, NONE };

// ---------------------------------------------------------------------------
// Per-thread RNG state. Each worker seeds this once at startup, so threads
// never share rand() state. It lives here rather than in the worker because
// Event's methods simulate service time while holding the lock, and routing
// that through a thread-local keeps the method signatures free of a seed
// parameter.
// ---------------------------------------------------------------------------
extern thread_local unsigned int rng_seed;

// Simulated query service time, called while the event lock is held.
void simulate_service_time();

// ---------------------------------------------------------------------------
// Event: owns its seat state and the reader-writer lock that guards it.
//
// Exclusion is fine-grained and per-event. Inquiries take the lock shared, so
// any number of them can read one event at once; bookings and cancellations
// take it exclusively, so a writer excludes every other reader and writer on
// that event. Two different events never contend, because they are two
// different locks.
//
// Every method uses the *try* form of the lock. On contention it returns false
// immediately instead of blocking, which is what drives the caller's
// back-off-and-retry path.
// ---------------------------------------------------------------------------
class Event
{
public:
    Event(int id, int capacity);
    ~Event();

    // A pthread_rwlock_t must not be copied: a copy would alias the same
    // underlying lock state and silently break mutual exclusion.
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;

    // Shared (read) access. Returns false if the lock was contended.
    bool try_inquire(int &seats_out);

    // Exclusive (write) access. Returns false if the lock was contended.
    // On success, `booked` reports whether there were enough seats.
    bool try_book(int k, bool &booked);

    // Exclusive (write) access. Returns false if the lock was contended.
    bool try_cancel(int k);

    // Lock-free accessors for the end-of-run invariant check ONLY. These are
    // safe solely because main() calls them after pthread_join, when the
    // process is single-threaded again. Never call them from a worker.
    int id_unsafe() const { return id; }
    int seats_unsafe() const { return available_seats; }

private:
    int id;
    int available_seats;
    pthread_rwlock_t rwlock;
};

// Event is non-copyable and non-movable, so the events are held by pointer.
extern std::vector<std::unique_ptr<Event> > events;

// ---------------------------------------------------------------------------
// System-wide concurrency cap. These guard the active-query counter and
// nothing else -- all data exclusion now lives inside Event.
// ---------------------------------------------------------------------------
extern pthread_mutex_t cap_mutex;
extern pthread_cond_t active_queries_cond;
extern int current_active_queries;

extern std::atomic<bool> system_running;

// ---------------------------------------------------------------------------
// macOS does not ship pthread_barrier_t, so shutdown uses a hand-rolled one.
// ---------------------------------------------------------------------------
struct MacBarrier
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int crossing;
};

extern MacBarrier thread_barrier;

void barrier_init(MacBarrier *barrier, int count);
void barrier_wait(MacBarrier *barrier);

#endif
