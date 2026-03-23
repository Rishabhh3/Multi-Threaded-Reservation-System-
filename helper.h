#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>
#include <vector>
#include "shared_table.h"

// Track a thread's private bookings for cancellations
struct BookingRecord {
    int event_id;
    int tickets_booked;
};

// Thread argument structure
struct ThreadArg {
    int thread_id;
};

// Main worker thread routine
void* worker_thread(void* arg);

#endif