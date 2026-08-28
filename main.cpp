#include "shared_table.h"
#include "helper.h"
#include <iostream>
#include <unistd.h>

bool system_running = true;

int main(int argc, char *argv[])
{
    // simple flag parsing: -e, -c, -m (MAX), -s (threads), -t (seconds)
    for (int i = 1; i < argc - 1; i++)
    {
        std::string flag = argv[i];
        int val = atoi(argv[i + 1]);
        if (flag == "-e")
            NUM_EVENTS = val;
        else if (flag == "-c")
            CAPACITY = val;
        else if (flag == "-m")
            MAX_ACTIVE_QUERIES = val;
        else if (flag == "-s")
            NUM_THREADS = val;
        else if (flag == "-t")
            RUN_DURATION_SEC = val;
    }

    events.resize(NUM_EVENTS);
    shared_table.resize(MAX_ACTIVE_QUERIES);

    barrier_init(&thread_barrier, NUM_THREADS + 1);

    for (int i = 0; i < NUM_EVENTS; i++)
    {
        events[i].id = i;
        events[i].available_seats = CAPACITY;
    }

    // NUM_THREADS is now a runtime value, so this must be a heap array
    // (a plain C array size must be known at compile time)
    pthread_t *threads = new pthread_t[NUM_THREADS];
    ThreadArg *args = new ThreadArg[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
    {
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    sleep(RUN_DURATION_SEC);
    system_running = false;

    barrier_wait(&thread_barrier);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    std::cout << "\nFinal Reservation Status:\n";
    for (int i = 0; i < NUM_EVENTS; i++)
        std::cout << "Event " << i << " available seats: " << events[i].available_seats << "\n";

    delete[] threads;
    delete[] args;
    return 0;
}