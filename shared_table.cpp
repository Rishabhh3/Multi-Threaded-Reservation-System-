#include "shared_table.h"

#include <cstdlib>
#include <unistd.h>

using namespace std;

int NUM_EVENTS = 100;
int CAPACITY = 500;
int MAX_ACTIVE_QUERIES = 5;
int NUM_THREADS = 20;
int RUN_DURATION_SEC = 60;
bool quiet_mode = false;

thread_local unsigned int rng_seed = 0;

vector<unique_ptr<Event> > events;   // filled in main()

// Cap bookkeeping. cap_mutex guards current_active_queries and nothing else.
pthread_mutex_t cap_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t active_queries_cond = PTHREAD_COND_INITIALIZER;
int current_active_queries = 0;

atomic<bool> system_running(true);

MacBarrier thread_barrier;

// ---------------------------------------------------------------------------
// Simulated service time.
//
// This runs *inside* the event lock on purpose. The lock would otherwise be
// held for a few hundred nanoseconds, contention would be effectively zero,
// and the retry-rate metric would read 0% at every MAX -- i.e. the benchmark
// would measure nothing. Holding the lock across the simulated work is what
// makes the read/write exclusion observable.
// ---------------------------------------------------------------------------
void simulate_service_time()
{
    usleep(rand_r(&rng_seed) % 50000 + 10000);
}

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

Event::Event(int id, int capacity)
    : id(id), available_seats(capacity)
{
    pthread_rwlock_init(&rwlock, NULL);
}

Event::~Event()
{
    pthread_rwlock_destroy(&rwlock);
}

bool Event::try_inquire(int &seats_out)
{
    if (pthread_rwlock_tryrdlock(&rwlock) != 0)
        return false;   // contended by a writer -- caller retries

    simulate_service_time();
    seats_out = available_seats;

    pthread_rwlock_unlock(&rwlock);
    return true;
}

bool Event::try_book(int k, bool &booked)
{
    if (pthread_rwlock_trywrlock(&rwlock) != 0)
        return false;   // contended by any reader or writer -- caller retries

    simulate_service_time();
    if (available_seats >= k)
    {
        available_seats -= k;
        booked = true;
    }
    else
    {
        booked = false;
    }

    pthread_rwlock_unlock(&rwlock);
    return true;
}

bool Event::try_cancel(int k)
{
    if (pthread_rwlock_trywrlock(&rwlock) != 0)
        return false;   // contended by any reader or writer -- caller retries

    simulate_service_time();
    available_seats += k;

    pthread_rwlock_unlock(&rwlock);
    return true;
}

// ---------------------------------------------------------------------------
// Hand-rolled barrier (macOS has no pthread_barrier_t)
// ---------------------------------------------------------------------------

void barrier_init(MacBarrier *barrier, int count)
{
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->count = count;
    barrier->crossing = 0;
}

void barrier_wait(MacBarrier *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    barrier->crossing++;
    if (barrier->crossing >= barrier->count)
    {
        barrier->crossing = 0;
        pthread_cond_broadcast(&barrier->cond);
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
}
