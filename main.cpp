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

    long total_attempted = 0, total_succeeded = 0, total_retried = 0, total_wait_usec = 0;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        total_attempted += args[i].stats.queries_attempted;
        total_succeeded += args[i].stats.queries_succeeded;
        total_retried += args[i].stats.queries_retried;
        total_wait_usec += args[i].stats.total_wait_usec;
    }

    std::cout << "\n=== Run Statistics ===\n";
    std::cout << "Total queries attempted: " << total_attempted << "\n";
    std::cout << "Total queries succeeded: " << total_succeeded << "\n";
    std::cout << "Total queries retried (admission conflict): " << total_retried << "\n";
    if (total_attempted > 0)
    {
        std::cout << "Retry rate: " << (100.0 * total_retried / total_attempted) << "%\n";
    }
    std::cout << "Total thread-time spent waiting on MAX cap: "
              << (total_wait_usec / 1000000.0) << " sec\n";

    // Invariant check — this is the actual proof the locking is correct.
    bool invariant_ok = true;
    for (int i = 0; i < NUM_EVENTS; i++)
    {
        if (events[i].available_seats < 0 || events[i].available_seats > CAPACITY)
        {
            std::cout << "INVARIANT VIOLATION: Event " << i << " has "
                      << events[i].available_seats << " seats (capacity " << CAPACITY << ")\n";
            invariant_ok = false;
        }
    }
    std::cout << "Invariant check: " << (invariant_ok ? "PASSED" : "FAILED") << "\n";

    std::cout << "\nFinal Reservation Status:\n";
    for (int i = 0; i < NUM_EVENTS; i++)
        std::cout << "Event " << i << " available seats: " << events[i].available_seats << "\n";

    std::cout << "\nFinal Reservation Status:\n";
    for (int i = 0; i < NUM_EVENTS; i++)
        std::cout << "Event " << i << " available seats: " << events[i].available_seats << "\n";

    delete[] threads;
    delete[] args;
    return 0;
}