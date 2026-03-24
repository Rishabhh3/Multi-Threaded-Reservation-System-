#include "shared_table.h"
#include "helper.h"
#include <iostream>
#include <unistd.h>

bool system_running = true;

int main() {
    // Initialize barrier for master + workers [cite: 130]
    barrier_init(&thread_barrier, NUM_THREADS + 1); 

    // Initialize events [cite: 85, 86]
    for (int i = 0; i < NUM_EVENTS; i++) {
        events[i].id = i;
        events[i].available_seats = CAPACITY; 
    }

    pthread_t threads[NUM_THREADS];
    ThreadArg args[NUM_THREADS];

    // Create worker threads [cite: 86]
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    // Run for predetermined duration T [cite: 127]
    sleep(60); 
    system_running = false; // Trigger shutdown [cite: 128]

    // Master thread waits at the barrier [cite: 130]
    barrier_wait(&thread_barrier); 

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL); // Terminate threads [cite: 129]
    }

    // Print final reservation status [cite: 132]
    std::cout << "\nFinal Reservation Status:\n";
    for (int i = 0; i < NUM_EVENTS; i++) {
        std::cout << "Event " << i << " available seats: " << events[i].available_seats << "\n";
    }

    return 0;
}