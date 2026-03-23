
#ifndef SHARED_TABLE_H
#define SHARED_TABLE_H

#include <vector>
#include <pthread.h>

const int NUM_EVENTS = 100;
const int CAPACITY = 500;
const int MAX_ACTIVE_QUERIES = 5;
const int NUM_THREADS = 20;

struct Event {
    int id;
    int available_seats;
};

enum QueryType { INQUIRY, BOOKING, CANCELLATION, NONE };

struct QueryEntry {
    int event_number = -1; 
    QueryType type = NONE;
    int thread_id = -1;
};

// Global data
extern std::vector<Event> events;
extern std::vector<QueryEntry> shared_table;

// Synchronization primitives
extern pthread_mutex_t table_mutex;
extern pthread_cond_t active_queries_cond;
extern int current_active_queries;

// Custom Barrier for macOS (since pthread_barrier_t isn't native)
struct MacBarrier {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int crossing;
};

extern MacBarrier thread_barrier;

void barrier_init(MacBarrier *barrier, int count);
void barrier_wait(MacBarrier *barrier);

#endif

bool can_admit_query(int event_id, QueryType type);
int add_query_to_table(int event_id, QueryType type, int thread_id);
void remove_query_from_table(int index);