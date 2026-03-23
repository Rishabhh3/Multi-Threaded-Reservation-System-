#include "helper.h"
#include <iostream>
#include <unistd.h>
#include <cstdlib>

extern bool system_running; 

void* worker_thread(void* arg) {
    ThreadArg* t_arg = (ThreadArg*)arg;
    int id = t_arg->thread_id;
    std::vector<BookingRecord> private_bookings; // [cite: 137]

    while (system_running) {
        QueryType type = (QueryType)(rand() % 3); // [cite: 89, 90]
        int event_id = rand() % NUM_EVENTS; // [cite: 91]
        int k = 5 + (rand() % 6); // Random 5 to 10 [cite: 92]
        
        if (type == CANCELLATION && private_bookings.empty()) type = INQUIRY;

        pthread_mutex_lock(&table_mutex);
        
        // Limit concurrent queries 
        while (current_active_queries >= MAX_ACTIVE_QUERIES && system_running) {
            std::cout << "Thread " << id << " waiting (MAX active).\n"; // [cite: 142, 143]
            pthread_cond_wait(&active_queries_cond, &table_mutex); // [cite: 99, 100]
        }
        if (!system_running) { pthread_mutex_unlock(&table_mutex); break; }

        // Enforce read/write consistency 
        if (!can_admit_query(event_id, type)) {
            pthread_mutex_unlock(&table_mutex);
            usleep(10000); // Sleep and try another [cite: 124, 125]
            continue;
        }

        int table_idx = add_query_to_table(event_id, type, id); // [cite: 121, 122]
        current_active_queries++;
        pthread_mutex_unlock(&table_mutex);

        std::cout << "Thread " << id << " started query type " << type << " on event " << event_id << "\n"; // [cite: 139, 140, 141]
        usleep(rand() % 50000 + 10000); // Simulate transaction delay [cite: 94]

        // TODO: Execute actual Booking/Cancellation/Inquiry logic on events[event_id]

        pthread_mutex_lock(&table_mutex);
        remove_query_from_table(table_idx); // [cite: 123]
        current_active_queries--;
        pthread_cond_signal(&active_queries_cond); // Signal waiting threads [cite: 100, 143]
        pthread_mutex_unlock(&table_mutex);

        usleep(rand() % 50000 + 10000); // Sleep between queries [cite: 95]
    }
    return NULL;
}