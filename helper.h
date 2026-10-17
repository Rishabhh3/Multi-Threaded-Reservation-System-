#ifndef HELPER_H
#define HELPER_H

#include <pthread.h>
#include <vector>
#include "shared_table.h"

struct BookingRecord
{
    int event_id;
    int tickets_booked;
};

// Per-thread stats -- only ever written by the owning thread, read by main()
// after pthread_join, so no locking is needed.
struct ThreadStats
{
    long queries_attempted = 0;
    long queries_succeeded = 0;  // acquired the event lock and executed; a booking
                                 // rejected for insufficient seats still counts here
    long queries_retried = 0;    // event lock was contended, backed off and retried
    long total_wait_usec = 0;    // time spent blocked on the MAX cap condvar

    // End-to-end latency per completed query, in microseconds. The clock starts
    // when a query is first generated and stops when one completes, so any
    // retries in between are charged to that sample.
    std::vector<long> latency_usec;
};

struct ThreadArg
{
    int thread_id;
    ThreadStats stats;
};

void *worker_thread(void *arg);

#endif
