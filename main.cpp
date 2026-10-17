#include "shared_table.h"
#include "helper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

static void usage(const char *prog)
{
    std::cerr
        << "Usage: " << prog << " [-e events] [-c capacity] [-m max_active]\n"
        << "       " << " " << "       [-s threads] [-t seconds] [-q]\n\n"
        << "  -e  number of events            (default 100)\n"
        << "  -c  seat capacity per event     (default 500)\n"
        << "  -m  max concurrent queries      (default 5)\n"
        << "  -s  number of worker threads    (default 20)\n"
        << "  -t  run duration in seconds     (default 60)\n"
        << "  -q  quiet: suppress per-query logging and the per-event dump\n\n"
        << "All numeric values must be greater than 0.\n";
}

// Nearest-rank percentile over a sorted vector of microsecond samples,
// returned in milliseconds.
static double percentile_ms(const std::vector<long> &sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    size_t idx = (size_t)std::ceil(p / 100.0 * (double)sorted.size());
    if (idx > 0)
        idx--;
    if (idx >= sorted.size())
        idx = sorted.size() - 1;
    return (double)sorted[idx] / 1000.0;
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc;)
    {
        std::string flag = argv[i];

        if (flag == "-q")
        {
            quiet_mode = true;
            i += 1;
            continue;
        }
        if (flag == "-h" || flag == "--help")
        {
            usage(argv[0]);
            return 0;
        }

        if (i + 1 >= argc)
        {
            std::cerr << "Error: missing value for " << flag << "\n\n";
            usage(argv[0]);
            return 1;
        }

        int val = atoi(argv[i + 1]);
        if (val <= 0)
        {
            std::cerr << "Error: value for " << flag << " must be > 0 (got '"
                      << argv[i + 1] << "')\n\n";
            usage(argv[0]);
            return 1;
        }

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
        else
        {
            std::cerr << "Error: unknown flag '" << flag << "'\n\n";
            usage(argv[0]);
            return 1;
        }

        i += 2;
    }

    events.reserve(NUM_EVENTS);
    for (int i = 0; i < NUM_EVENTS; i++)
        events.push_back(std::unique_ptr<Event>(new Event(i, CAPACITY)));

    barrier_init(&thread_barrier, NUM_THREADS + 1);

    pthread_t *threads = new pthread_t[NUM_THREADS];
    ThreadArg *args = new ThreadArg[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
    {
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    sleep(RUN_DURATION_SEC);

    // Shut down. The flag is atomic, and the broadcast happens under cap_mutex
    // so a thread that is about to park on the condvar cannot miss it and sleep
    // through shutdown.
    system_running.store(false, std::memory_order_relaxed);
    pthread_mutex_lock(&cap_mutex);
    pthread_cond_broadcast(&active_queries_cond);
    pthread_mutex_unlock(&cap_mutex);

    barrier_wait(&thread_barrier);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    // ---- aggregate (single-threaded from here on) -------------------------
    long total_attempted = 0, total_succeeded = 0, total_retried = 0, total_wait_usec = 0;
    std::vector<long> latencies;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        total_attempted += args[i].stats.queries_attempted;
        total_succeeded += args[i].stats.queries_succeeded;
        total_retried += args[i].stats.queries_retried;
        total_wait_usec += args[i].stats.total_wait_usec;
        latencies.insert(latencies.end(),
                         args[i].stats.latency_usec.begin(),
                         args[i].stats.latency_usec.end());
    }
    std::sort(latencies.begin(), latencies.end());

    double lat_mean_ms = 0.0;
    if (!latencies.empty())
    {
        double sum = 0.0;
        for (size_t i = 0; i < latencies.size(); i++)
            sum += (double)latencies[i];
        lat_mean_ms = sum / (double)latencies.size() / 1000.0;
    }
    double lat_p50 = percentile_ms(latencies, 50.0);
    double lat_p95 = percentile_ms(latencies, 95.0);
    double lat_p99 = percentile_ms(latencies, 99.0);
    double lat_max = latencies.empty() ? 0.0 : (double)latencies.back() / 1000.0;

    double retry_rate = total_attempted > 0
                            ? 100.0 * (double)total_retried / (double)total_attempted
                            : 0.0;
    double qps = (double)total_succeeded / (double)RUN_DURATION_SEC;
    double cap_wait_s = (double)total_wait_usec / 1000000.0;

    std::cout << "\n=== Run Statistics ===\n";
    std::cout << "Config: events=" << NUM_EVENTS << " capacity=" << CAPACITY
              << " max_active=" << MAX_ACTIVE_QUERIES << " threads=" << NUM_THREADS
              << " duration=" << RUN_DURATION_SEC << "s\n";
    std::cout << "Total queries attempted: " << total_attempted << "\n";
    std::cout << "Total queries succeeded: " << total_succeeded << "\n";
    std::cout << "Total queries retried (lock contended): " << total_retried << "\n";
    std::cout << "Retry rate: " << retry_rate << "%\n";
    std::cout << "Throughput: " << qps << " queries/sec\n";
    std::cout << "Total thread-time spent waiting on MAX cap: " << cap_wait_s << " sec\n";
    std::cout << "Latency (end-to-end, retries included), ms: "
              << "mean=" << lat_mean_ms
              << " p50=" << lat_p50
              << " p95=" << lat_p95
              << " p99=" << lat_p99
              << " max=" << lat_max << "\n";

    // Invariant check -- the actual proof that the per-event locking held.
    bool invariant_ok = true;
    for (int i = 0; i < NUM_EVENTS; i++)
    {
        int seats = events[i]->seats_unsafe();
        if (seats < 0 || seats > CAPACITY)
        {
            std::cout << "INVARIANT VIOLATION: Event " << i << " has " << seats
                      << " seats (capacity " << CAPACITY << ")\n";
            invariant_ok = false;
        }
    }
    std::cout << "Invariant check: " << (invariant_ok ? "PASSED" : "FAILED") << "\n";

    // Single machine-readable line for bench.sh to parse.
    printf("RESULT,max=%d,threads=%d,events=%d,capacity=%d,duration=%d,"
           "attempted=%ld,succeeded=%ld,retried=%ld,retry_rate=%.4f,qps=%.4f,"
           "lat_mean_ms=%.4f,lat_p50_ms=%.4f,lat_p95_ms=%.4f,lat_p99_ms=%.4f,"
           "lat_max_ms=%.4f,cap_wait_s=%.4f,invariant=%s\n",
           MAX_ACTIVE_QUERIES, NUM_THREADS, NUM_EVENTS, CAPACITY, RUN_DURATION_SEC,
           total_attempted, total_succeeded, total_retried, retry_rate, qps,
           lat_mean_ms, lat_p50, lat_p95, lat_p99, lat_max, cap_wait_s,
           invariant_ok ? "PASSED" : "FAILED");

    if (!quiet_mode)
    {
        std::cout << "\nFinal Reservation Status:\n";
        for (int i = 0; i < NUM_EVENTS; i++)
            std::cout << "Event " << i << " available seats: "
                      << events[i]->seats_unsafe() << "\n";
    }

    delete[] threads;
    delete[] args;
    return invariant_ok ? 0 : 1;
}
