# Multi-Threaded Event Reservation System Architecture

## 1. File Structure
* [cite_start]**`shared_table.h` / `shared_table.cpp`**: Contains global shared data (events, query table) and synchronization primitives (mutex, condition variables, barrier)[cite: 98, 99, 110, 120, 131].
* [cite_start]**`worker.h` / `worker.cpp`**: Contains the `worker_thread` routine, local state (private bookings), and query execution logic (Inquiry, Booking, Cancellation)[cite: 87, 137].
* [cite_start]**`main.cpp`**: The master thread that initializes the system, spawns workers, manages the runtime timer `T`, and handles the shutdown barrier[cite: 84, 127, 130].

## 2. Core Components & Shared State
* [cite_start]**Events Array**: A vector of 100 events, each with an ID and `available_seats` (initialized to 500)[cite: 74, 75, 86].
* [cite_start]**Shared Query Table**: A table limited to `MAX` (5) entries tracking currently active queries (Event ID, Query Type, Thread ID)[cite: 80, 111, 112].
* [cite_start]**Private Bookings**: A local vector maintained independently by each worker thread to track its own successful bookings for future cancellations[cite: 137, 138].

## 3. Synchronization Mechanisms
* [cite_start]**`table_mutex`**: A single mutex protecting access to the Shared Query Table, the Events array, and the active queries counter[cite: 120].
* **`active_queries_cond`**: A condition variable that blocks worker threads when `current_active_queries >= MAX`, preventing system overload[cite: 97, 98, 99].
* **`thread_barrier`**: A custom implementation of a thread barrier used during system shutdown. [cite_start]The master thread waits here until all worker threads complete their execution[cite: 130, 131].

## 4. Query Execution Flow
1. [cite_start]**Generate**: Thread generates a random query type, event ID, and ticket count `k`[cite: 89, 90, 91, 92].
2. **Admit**: Thread locks `table_mutex`. It waits on `active_queries_cond` if the system is at `MAX` capacity[cite: 97, 98]. [cite_start]It then checks read/write consistency rules against the Shared Query Table[cite: 118, 119]. 
3. [cite_start]**Execute**: If admitted, it adds itself to the table, releases the lock, sleeps to simulate delay, and performs the inquiry/booking/cancellation[cite: 94, 121, 122].
4. [cite_start]**Clean up**: Thread re-locks `table_mutex`, removes its table entry, signals `active_queries_cond` to wake waiting threads, and unlocks[cite: 100, 123].