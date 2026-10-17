#include "helper.h"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sys/time.h>
#include <unistd.h>

static long usec_since(const timeval &start)
{
    timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - start.tv_sec) * 1000000L + (now.tv_usec - start.tv_usec);
}

// Give up a cap slot and wake anyone parked on the cap. This must broadcast on
// every release, including the retry path: a slot that is freed without a wakeup
// can leave waiters parked even though the system is no longer at MAX.
static void release_cap_slot()
{
    pthread_mutex_lock(&cap_mutex);
    current_active_queries--;
    pthread_cond_broadcast(&active_queries_cond);
    pthread_mutex_unlock(&cap_mutex);
}

void *worker_thread(void *arg)
{
    ThreadArg *t_arg = (ThreadArg *)arg;
    int id = t_arg->thread_id;
    ThreadStats &stats = t_arg->stats;

    // Per-thread RNG state: no shared rand() state between workers.
    rng_seed = (unsigned int)(time(NULL) ^ id);

    std::vector<BookingRecord> private_bookings;

    // Reserve generously so push_back never allocates mid-measurement. A thread
    // cannot complete more than roughly 1/0.07s = 14 queries per second given
    // the simulated service and post-query sleeps, so this is ~14x headroom.
    stats.latency_usec.reserve((size_t)RUN_DURATION_SEC * 200 + 1024);

    // Latency clock. It is started when a query is generated and only reset
    // after one completes, so a query that gets bounced and retried carries its
    // original start time and is charged for the time its retries cost.
    bool timing = false;
    timeval query_start;

    while (system_running.load(std::memory_order_relaxed))
    {
        if (!timing)
        {
            gettimeofday(&query_start, NULL);
            timing = true;
        }

        QueryType type = (QueryType)(rand_r(&rng_seed) % 3);
        int k = 5 + (rand_r(&rng_seed) % 6);

        if (type == CANCELLATION && private_bookings.empty())
            type = INQUIRY;

        // Resolve the real target event BEFORE taking any lock. A cancellation
        // must act on an event drawn from this thread's own booking list, never
        // on a fresh random id -- otherwise the lock taken and the state mutated
        // are two different events and the locking protects nothing.
        int event_id;
        int cancel_idx = -1;
        if (type == CANCELLATION)
        {
            cancel_idx = rand_r(&rng_seed) % private_bookings.size();
            event_id = private_bookings[cancel_idx].event_id;
        }
        else
        {
            event_id = rand_r(&rng_seed) % NUM_EVENTS;
        }

        stats.queries_attempted++;

        // ---- acquire a cap slot (blocking) --------------------------------
        pthread_mutex_lock(&cap_mutex);
        while (current_active_queries >= MAX_ACTIVE_QUERIES &&
               system_running.load(std::memory_order_relaxed))
        {
            timeval wait_start;
            gettimeofday(&wait_start, NULL);
            pthread_cond_wait(&active_queries_cond, &cap_mutex);
            stats.total_wait_usec += usec_since(wait_start);
        }
        if (!system_running.load(std::memory_order_relaxed))
        {
            pthread_mutex_unlock(&cap_mutex);
            break;
        }
        current_active_queries++;
        pthread_mutex_unlock(&cap_mutex);

        // ---- deadlock freedom ---------------------------------------------
        // cap_mutex is released above, before any event lock is taken. A thread
        // therefore never holds cap_mutex and an event rwlock at the same time,
        // and it holds at most one event rwlock at a time (one query, one
        // event). With no thread ever holding two locks, no hold-and-wait cycle
        // can form, so the design is deadlock-free by construction. The event
        // locks are also only ever taken with try*, so even a cycle could not
        // block.
        // -------------------------------------------------------------------

        bool acquired = false;
        bool booked = false;
        int seats_seen = 0;
        int cancel_tickets = 0;

        if (type == INQUIRY)
        {
            acquired = events[event_id]->try_inquire(seats_seen);
        }
        else if (type == BOOKING)
        {
            acquired = events[event_id]->try_book(k, booked);
        }
        else // CANCELLATION
        {
            cancel_tickets = private_bookings[cancel_idx].tickets_booked;
            acquired = events[event_id]->try_cancel(cancel_tickets);
        }

        release_cap_slot();

        if (!acquired)
        {
            // Event lock was contended. Do not block on it: back off and come
            // back with a freshly generated query. The latency clock keeps
            // running, so this retry is charged to the pending sample.
            stats.queries_retried++;
            usleep(10000);
            continue;
        }

        // ---- the query ran; update thread-local bookkeeping ---------------
        if (type == BOOKING && booked)
        {
            private_bookings.push_back({event_id, k});
        }
        else if (type == CANCELLATION)
        {
            private_bookings.erase(private_bookings.begin() + cancel_idx);
        }

        stats.queries_succeeded++;
        stats.latency_usec.push_back(usec_since(query_start));
        timing = false;

        if (!quiet_mode)
        {
            if (type == INQUIRY)
            {
                std::cout << "Thread " << id << " Inquiry: Event " << event_id
                          << " has " << seats_seen << " seats.\n";
            }
            else if (type == BOOKING)
            {
                std::cout << "Thread " << id << " Booking: "
                          << (booked ? "Success" : "Failed (insufficient seats)")
                          << " for Event " << event_id << " (" << k << " tickets).\n";
            }
            else
            {
                std::cout << "Thread " << id << " Cancel: Freed " << cancel_tickets
                          << " tickets for Event " << event_id << ".\n";
            }
        }

        usleep(rand_r(&rng_seed) % 50000 + 10000);
    }

    if (!quiet_mode)
        std::cout << "Thread " << id << " terminating.\n";

    barrier_wait(&thread_barrier);
    return NULL;
}
