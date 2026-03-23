
#include "shared_table.h"

// Initialize global vectors
std::vector<Event> events(NUM_EVENTS);
std::vector<QueryEntry> shared_table(MAX_ACTIVE_QUERIES);

// Initialize synchronization primitives
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t active_queries_cond = PTHREAD_COND_INITIALIZER;
int current_active_queries = 0;

MacBarrier thread_barrier;

void barrier_init(MacBarrier *barrier, int count) {
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->count = count;
    barrier->crossing = 0;
}

void barrier_wait(MacBarrier *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->crossing++;
    if (barrier->crossing >= barrier->count) {
        barrier->crossing = 0;
        pthread_cond_broadcast(&barrier->cond);
    } else {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
}
bool can_admit_query(int event_id, QueryType type) {
    for (int i = 0; i < MAX_ACTIVE_QUERIES; ++i) {
        if (shared_table[i].event_number == event_id) {
            // Read query blocked by active write 
            if (type == INQUIRY && (shared_table[i].type == BOOKING || shared_table[i].type == CANCELLATION)) {
                return false; 
            }
            // Write query blocked by any active read or write 
            if ((type == BOOKING || type == CANCELLATION) && shared_table[i].type != NONE) {
                return false; 
            }
        }
    }
    return true;
}

int add_query_to_table(int event_id, QueryType type, int thread_id) {
    for (int i = 0; i < MAX_ACTIVE_QUERIES; ++i) {
        if (shared_table[i].event_number == -1) { // Find a blank entry [cite: 116]
            shared_table[i] = {event_id, type, thread_id}; // Fill the entry [cite: 122]
            return i;
        }
    }
    return -1;
}

void remove_query_from_table(int index) {
    shared_table[index].event_number = -1; // Remove entry after completing query [cite: 123]
    shared_table[index].type = NONE;
    shared_table[index].thread_id = -1;
}