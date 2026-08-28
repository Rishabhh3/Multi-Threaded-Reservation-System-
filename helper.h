#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>
#include <vector>
#include "shared_table.h"

struct BookingRecord
{
    int event_id;
    int tickets_booked;
};

// Per-thread stats — only ever written by the owning thread,
// read by main() after pthread_join, so no locking needed.
struct ThreadStats
{
    long queries_attempted = 0;
    long queries_succeeded = 0;
    long queries_retried = 0; // failed admission, went back to sleep+retry
    long total_wait_usec = 0; // time spent blocked on active_queries_cond
};

struct ThreadArg
{
    int thread_id;
    ThreadStats stats;
};

void *worker_thread(void *arg);

#endif