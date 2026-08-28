#include "helper.h"
#include <iostream>
#include <unistd.h>
#include <cstdlib>

extern bool system_running;

void *worker_thread(void *arg)
{
    ThreadArg *t_arg = (ThreadArg *)arg;
    int id = t_arg->thread_id;
    std::vector<BookingRecord> private_bookings;

    while (system_running)
    {
        QueryType type = (QueryType)(rand() % 3);
        int k = 5 + (rand() % 6);

        if (type == CANCELLATION && private_bookings.empty())
            type = INQUIRY;

        // Decide the *actual* target event_id up front, before any admission
        // check — for CANCELLATION this must be the event of the booking
        // we're about to cancel, not an unrelated random pick.
        int event_id;
        int cancel_idx = -1;
        if (type == CANCELLATION)
        {
            cancel_idx = rand() % private_bookings.size();
            event_id = private_bookings[cancel_idx].event_id;
        }
        else
        {
            event_id = rand() % NUM_EVENTS;
        }

        pthread_mutex_lock(&table_mutex);

        while (current_active_queries >= MAX_ACTIVE_QUERIES && system_running)
        {
            std::cout << "Thread " << id << " waiting (MAX active).\n";
            pthread_cond_wait(&active_queries_cond, &table_mutex);
        }
        if (!system_running)
        {
            pthread_mutex_unlock(&table_mutex);
            break;
        }

        if (!can_admit_query(event_id, type))
        {
            pthread_mutex_unlock(&table_mutex);
            usleep(10000);
            continue;
        }

        int table_idx = add_query_to_table(event_id, type, id);
        current_active_queries++;
        pthread_mutex_unlock(&table_mutex);

        std::cout << "Thread " << id << " started query type " << type << " on event " << event_id << "\n";
        usleep(rand() % 50000 + 10000);

        if (type == INQUIRY)
        {
            std::cout << "Thread " << id << " Inquiry result: Event " << event_id
                      << " has " << events[event_id].available_seats << " seats.\n";
        }
        else if (type == BOOKING)
        {
            if (events[event_id].available_seats >= k)
            {
                events[event_id].available_seats -= k;
                private_bookings.push_back({event_id, k});
                std::cout << "Thread " << id << " Booking result: Success for Event "
                          << event_id << " (" << k << " tickets).\n";
            }
            else
            {
                std::cout << "Thread " << id << " Booking result: Failed (Insufficient seats) for Event "
                          << event_id << ".\n";
            }
        }
        else if (type == CANCELLATION)
        {
            int cancel_tickets = private_bookings[cancel_idx].tickets_booked;
            events[event_id].available_seats += cancel_tickets; // same event_id as admission
            private_bookings.erase(private_bookings.begin() + cancel_idx);
            std::cout << "Thread " << id << " Cancel result: Freed " << cancel_tickets
                      << " tickets for Event " << event_id << ".\n";
        }

        pthread_mutex_lock(&table_mutex);
        remove_query_from_table(table_idx);
        current_active_queries--;
        std::cout << "Thread " << id << " signaling (query complete).\n";
        pthread_cond_signal(&active_queries_cond);
        pthread_mutex_unlock(&table_mutex);

        usleep(rand() % 50000 + 10000);
    }

    std::cout << "Thread " << id << " terminating due to timeout.\n";
    barrier_wait(&thread_barrier);
    return NULL;
}