#include "hp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

static const char* LEVEL_STRINGS[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

/* ============================================================================
 * Thread-local Storage for Thread ID
 * ============================================================================ */

static __thread uint32_t tls_thread_id = 0;
static _Atomic uint32_t next_thread_id = 1;

static inline uint32_t get_thread_id(void) {
    if (tls_thread_id == 0) {
        tls_thread_id = atomic_fetch_add(&next_thread_id, 1);
    }
    return tls_thread_id;
}

/* ============================================================================
 * TASK 1: Ring Buffer Initialization (10 pts)
 * 
 * Initialize the ring buffer structure:
 * - Zero out the entire structure
 * - Set each entry's sequence number to its index (0, 1, 2, ...)
 * - Initialize head, tail, and statistics to 0
 * 
 * HINT: Use atomic_init() for atomic variables
 * ============================================================================ */
static void ringbuf_init(hplog_ringbuf_t* rb) {
    if (!rb) {
        return;
    }

    memset(rb, 0, sizeof(*rb));
    for (size_t i = 0; i < HPLOG_BUFFER_SIZE; i++) {
        atomic_init(&rb->entries[i].sequence, (uint32_t)i);
    }
    
    atomic_init(&rb->head, 0);
    atomic_init(&rb->tail, 0);
    atomic_init(&rb->total_produced, 0);
    atomic_init(&rb->total_consumed, 0);
    atomic_init(&rb->dropped_count, 0);
}

/* ============================================================================
 * TASK 2a: Producer - Reserve Slot (20 pts)
 * 
 * Try to reserve a slot in the ring buffer for writing.
 * This must be LOCK-FREE and support multiple concurrent producers.
 * 
 * Algorithm:
 * 1. Load current head position
 * 2. Get entry at (head & MASK)
 * 3. Load entry's sequence number
 * 4. If sequence == (head & 0xFFFFFFFF), slot is available
 *    - Try to CAS head from current to current+1
 *    - If CAS succeeds, return the entry pointer
 *    - If CAS fails, retry from step 1
 * 5. If sequence < head (signed comparison), buffer is full
 *    - Return NULL
 * 6. Otherwise, another producer claimed this slot, retry
 * 
 * HINTS:
 * - Use atomic_load_explicit with memory_order_relaxed for head
 * - Use atomic_load_explicit with memory_order_acquire for sequence
 * - Use atomic_compare_exchange_weak_explicit for the CAS
 * 
 * @return Pointer to reserved entry, or NULL if buffer full
 * ============================================================================ */
static hplog_entry_t* ringbuf_try_reserve(hplog_ringbuf_t* rb) {
    while (1) {
        uint64_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
        hplog_entry_t* entry = &rb->entries[head & HPLOG_BUFFER_MASK];
        uint32_t seq = atomic_load_explicit(&entry->sequence, memory_order_acquire);

        if (seq == (uint32_t)head) {
            if (atomic_compare_exchange_weak_explicit(&rb->head, &head, head + 1, memory_order_acq_rel, memory_order_relaxed)) {
                return entry;
            }
        }
        else if ((int32_t)(seq - (uint32_t)head) < 0) {
            return NULL;
        }
        else {
            continue;
        }
    }
}

/* ============================================================================
 * TASK 2b: Producer - Commit Entry (10 pts)
 * 
 * Commit a previously reserved entry, making it visible to the consumer.
 * 
 * The sequence number indicates entry state:
 * - seq == index: slot is free for producer
 * - seq == index + 1: slot contains data for consumer
 * 
 * To commit:
 * 1. Calculate the new sequence: (entry_index + 1)
 * 2. Store with memory_order_release to ensure all writes are visible
 * 3. Increment total_produced counter
 * 
 * @param rb    Ring buffer
 * @param entry Entry that was previously reserved
 * ============================================================================ */
static void ringbuf_commit(hplog_ringbuf_t* rb, hplog_entry_t* entry) {
    uint32_t i = (uint32_t)(entry - rb->entries);
    atomic_store_explicit(&entry->sequence, i + 1, memory_order_release);
    atomic_fetch_add_explicit(&rb->total_produced, 1, memory_order_release);
}

/* ============================================================================
 * TASK 3a: Consumer - Try Dequeue (10 pts)
 * 
 * Check if an entry is ready to be consumed.
 * 
 * Algorithm:
 * 1. Load current tail position
 * 2. Get entry at (tail & MASK)
 * 3. Load entry's sequence
 * 4. If sequence == (tail & MASK) + 1, entry is ready
 *    - Return entry pointer
 * 5. Otherwise, return NULL (no data available)
 * 
 * NOTE: Only ONE consumer thread calls this, so no CAS needed for tail.
 * 
 * @return Pointer to ready entry, or NULL if buffer empty
 * ============================================================================ */
static hplog_entry_t* ringbuf_try_dequeue(hplog_ringbuf_t* rb) {
    uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    hplog_entry_t *entry = &rb->entries[tail & HPLOG_BUFFER_MASK];
    uint32_t seq = atomic_load_explicit(&entry->sequence, memory_order_acquire);
    
    if (seq == (tail & HPLOG_BUFFER_MASK) + 1) {
        return entry;
    }

    return NULL;
}

/* ============================================================================
 * TASK 3b: Consumer - Release Entry (10 pts)
 * 
 * Release an entry after consuming it, making slot available to producers.
 * 
 * To release:
 * 1. Increment tail (atomic fetch_add)
 * 2. Calculate new sequence: (old_tail + BUFFER_SIZE)
 *    This marks the slot as "available" for the next wrap-around
 * 3. Store sequence with release ordering
 * 4. Increment total_consumed
 * 
 * @param rb    Ring buffer
 * @param entry Entry to release
 * ============================================================================ */

static void ringbuf_release(hplog_ringbuf_t* rb, hplog_entry_t* entry) {
    uint64_t tail = atomic_fetch_add_explicit(&rb->tail, 1, memory_order_relaxed);
    uint32_t seq = tail + HPLOG_BUFFER_SIZE;
    atomic_store_explicit(&entry->sequence, seq, memory_order_release);
    atomic_fetch_add_explicit(&rb->total_consumed, 1, memory_order_release);
}

/* ============================================================================
 * TASK 4a: Memory-Mapped File - Grow (10 pts)
 * 
 * Grow the memory-mapped output file to a new size.
 * 
 * Steps:
 * 1. If file_map exists, msync and munmap it
 * 2. Use ftruncate to extend file to new_size
 * 3. mmap the file with PROT_READ|PROT_WRITE and MAP_SHARED
 * 4. Use madvise with MADV_SEQUENTIAL hint
 * 5. Update log->file_size
 * 
 * @return 0 on success, -1 on failure
 * ============================================================================ */
static int mmap_grow(hplog_t* log, size_t new_size) {
    if (log->file_map) {
        msync(log->file_map, log->file_size, MS_ASYNC);
        munmap(log->file_map, log->file_size);
        log->file_map = NULL;
    }

    if (ftruncate(log->fd, new_size) == -1)
        return -1;

    void* map = mmap(NULL, new_size, PROT_READ|PROT_WRITE, MAP_SHARED, log->fd, 0);
    if (map == MAP_FAILED)
        return -1;

    madvise(map, new_size, MADV_SEQUENTIAL);
    log->file_map = map;
    log->file_size = new_size;
    return 0;
}

/* ============================================================================
 * TASK 4b: Memory-Mapped File - Write (10 pts)
 * 
 * Write data to the memory-mapped file, growing if necessary.
 * 
 * Steps:
 * 1. Check if (file_offset + len) exceeds file_size
 * 2. If so, grow the file
 * 3. memcpy data to (file_map + file_offset)
 * 4. Update file_offset
 * 5. Atomically add to bytes_written
 * 
 * @return 0 on success, -1 on failure
 * ============================================================================ */
static int mmap_write(hplog_t* log, const char* data, size_t len) {
    if (log->file_offset + len > log->file_size || !log->file_map) {
        size_t new_size = log->file_size + HPLOG_FILE_GROW_SIZE;
        while (new_size < log->file_offset + len)
            new_size += HPLOG_FILE_GROW_SIZE;
        if (mmap_grow(log, new_size) == -1)
            return -1;
    }

    memcpy(log->file_map + log->file_offset, data, len);
    log->file_offset += len;
    atomic_fetch_add_explicit(&log->bytes_written, len, memory_order_relaxed);
    return 0;
}

/* ============================================================================
 * Format Entry Helper (PROVIDED)
 * ============================================================================ */
static int format_entry(char* buf, size_t buf_size, const hplog_entry_t* entry) {
    time_t sec = entry->timestamp_ns / 1000000000ULL;
    uint32_t nsec = entry->timestamp_ns % 1000000000ULL;
    
    struct tm tm;
    localtime_r(&sec, &tm);
    
    int level_idx = (entry->level < HPLOG_LEVEL_COUNT) ? entry->level : HPLOG_INFO;
    
    int len = snprintf(buf, buf_size,
                       "%04d-%02d-%02d %02d:%02d:%02d.%09u [%s] [T%03u] %.*s\n",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, nsec,
                       LEVEL_STRINGS[level_idx],
                       entry->thread_id,
                       entry->msg_len, entry->message);
    
    return (len > 0 && (size_t)len < buf_size) ? len : -1;
}

/* ============================================================================
 * TASK 5: Consumer Thread (15 pts)
 * 
 * Main loop for the consumer thread that drains the ring buffer to disk.
 * 
 * Algorithm:
 * 1. While running OR there are unconsumed entries:
 *    a. Try to dequeue an entry
 *    b. If entry available:
 *       - Format it using format_entry()
 *       - Write to mmap'd file using mmap_write()
 *       - Release the entry
 *       - Reset consecutive_empty counter
 *    c. If no entry:
 *       - Increment consecutive_empty
 *       - Adaptive wait:
 *         < 100: spin with pause instruction
 *         < 1000: sched_yield()
 *         else: nanosleep for a bit
 * 
 * 2. Final msync before returning
 * 
 * HINTS:
 * - Use __asm__ volatile("pause" ::: "memory") for spin
 * - Check both log->running AND (produced > consumed) for termination
 * ============================================================================ */
static void* consumer_thread_func(void* arg)
{
    hplog_t* log = (hplog_t*)arg;
    char format_buf[512];
    int consecutive_empty = 0;
    
    while (atomic_load_explicit(&log->running, memory_order_acquire) ||
           atomic_load_explicit(&log->ringbuf->total_produced, memory_order_acquire) >
           atomic_load_explicit(&log->ringbuf->total_consumed, memory_order_acquire)) {
        hplog_entry_t *entry = ringbuf_try_dequeue(log->ringbuf);

        if (entry) {
            int len = format_entry(format_buf, sizeof(format_buf), entry);

            if (len > 0) {
                mmap_write(log, format_buf, (size_t)len);
            }

            ringbuf_release(log->ringbuf, entry);
            consecutive_empty = 0;
        }
        else {
            consecutive_empty++;
            if (consecutive_empty < 100) {
  #if defined(__x86_64__) || defined(__i386__)
                __asm__ volatile("pause" ::: "memory");
  #elif defined(__aarch64__)
                __asm__ volatile("yield" ::: "memory");
  #elif defined(__arm__)
                __asm__ volatile("yield" ::: "memory");
  #else
                /* Fallback: compiler barrier only */
                __asm__ volatile("" ::: "memory");
  #endif
            } else if (consecutive_empty < 1000) {
                sched_yield();
            } else {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = HPLOG_CONSUMER_POLL_NS };
                nanosleep(&ts, NULL);
            }
        }
    }

    if (log->file_map) {
        msync(log->file_map, log->file_offset, MS_ASYNC);
    }
    
    return NULL;
}

/* ============================================================================
 * Initialization (PARTIALLY PROVIDED)
 * ============================================================================ */
hplog_t* hplog_init(const char* filepath, 
                    hplog_level_t min_level,
                    hplog_backpressure_t backpressure) {
    hplog_t* log = NULL;
    hplog_ringbuf_t* rb = NULL;
    
    /* Allocate logger context (cache-line aligned) */
    log = aligned_alloc(64, sizeof(hplog_t));
    if (!log) goto error;
    memset(log, 0, sizeof(*log));
    log->fd = -1;
    
    /* Allocate ring buffer (page-aligned) */
    rb = aligned_alloc(4096, sizeof(hplog_ringbuf_t));
    if (!rb) goto error;
    ringbuf_init(rb);
    log->ringbuf = rb;
    
    /* Store configuration */
    strncpy(log->filepath, filepath, sizeof(log->filepath) - 1);
    log->min_level = min_level;
    log->backpressure = backpressure;
    log->sync_on_flush = true;
    
    /* Open output file */
    log->fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (log->fd < 0) goto error;
    
    /* Initialize mmap'd file */
    if (mmap_grow(log, HPLOG_FILE_INITIAL_SIZE) < 0) goto error;
    
    /* Initialize atomics and start consumer thread */
    atomic_init(&log->running, true);
    atomic_init(&log->shutdown_requested, false);
    atomic_init(&log->bytes_written, 0);
    atomic_init(&log->flush_count, 0);
    
    if (pthread_create(&log->consumer_thread, NULL, 
                       consumer_thread_func, log) != 0) {
        goto error;
    }
    
    return log;
    
error:
    if (log) {
        if (log->file_map && log->file_map != MAP_FAILED) {
            munmap(log->file_map, log->file_size);
        }
        if (log->fd >= 0) close(log->fd);
        free(log);
    }
    if (rb) free(rb);
    return NULL;
}

/* ============================================================================
 * Shutdown (PROVIDED)
 * ============================================================================ */
void hplog_shutdown(hplog_t* log) {
    if (!log) return;
    
    atomic_store_explicit(&log->shutdown_requested, true, memory_order_release);

    /* Wait for in-flight producers to finish committing their reserved slots.
     * After shutdown_requested is visible, no new reservations can occur.
     * head == total_produced means every reserved slot has been committed. */
    while (atomic_load_explicit(&log->ringbuf->head, memory_order_acquire) !=
           atomic_load_explicit(&log->ringbuf->total_produced, memory_order_acquire)) {
        sched_yield();
    }

    atomic_store_explicit(&log->running, false, memory_order_release);
    
    pthread_join(log->consumer_thread, NULL);
    
    if (log->fd >= 0) {
        (void)ftruncate(log->fd, log->file_offset);
    }

    if (log->file_map && log->file_map != MAP_FAILED) {
        munmap(log->file_map, log->file_size);
    }
    if (log->fd >= 0) close(log->fd);
    if (log->ringbuf) free(log->ringbuf);
    free(log);
}

/* ============================================================================
 * Log Write (PARTIALLY PROVIDED)
 * 
 * You need to implement the backpressure retry loop.
 * ============================================================================ */
hplog_result_t hplog_write(hplog_t* log, hplog_level_t level, 
                           const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    uint64_t ts = hplog_timestamp_ns();
    
    /* Check level filter */
    if (level < log->min_level) {
        va_end(args);
        return HPLOG_OK;
    }
    
    /* Check shutdown */
    if (atomic_load(&log->shutdown_requested)) {
        va_end(args);
        return HPLOG_ERR_SHUTDOWN;
    }
    
    /* TODO: Try to reserve a slot with backpressure handling
     * 
     * Implement retry logic based on log->backpressure:
     * - HPLOG_BP_DROP: Try once, if fails increment dropped_count and return
     * - HPLOG_BP_SPIN: Retry with pause instruction (limit retries!)
     * - HPLOG_BP_YIELD: Retry with sched_yield()
     */
    hplog_entry_t* entry = NULL;

    switch (log->backpressure) {
        case HPLOG_BP_DROP:
            entry = ringbuf_try_reserve(log->ringbuf);
            break;
        case HPLOG_BP_SPIN:
            for (int retries = 0; retries < 100000; retries++) {
                entry = ringbuf_try_reserve(log->ringbuf);
                if (entry) break;
#if defined(__x86_64__) || defined(__i386__)
                    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
                    __asm__ volatile("yield" ::: "memory");
#else
                    __asm__ volatile("" ::: "memory");
#endif
            }
            break;
        case HPLOG_BP_YIELD:
            for (int retries = 0; retries < 100000; retries++) {
                entry = ringbuf_try_reserve(log->ringbuf);
                if (entry) 
                    break;
                sched_yield();
            }
        break;
    }

    if (entry == NULL) {
        atomic_fetch_add(&log->ringbuf->dropped_count, 1);
        va_end(args);
        return HPLOG_ERR_FULL;
    }
    
    /* Fill entry */
    entry->timestamp_ns = ts;
    entry->thread_id = get_thread_id();
    entry->level = (uint8_t)level;
    
    int len = vsnprintf(entry->message, HPLOG_MAX_MSG_LEN, fmt, args);
    entry->msg_len = (len > 0 && len < HPLOG_MAX_MSG_LEN) 
                     ? (uint16_t)len : HPLOG_MAX_MSG_LEN - 1;
    
    va_end(args);
    
    /* Commit the entry */
    ringbuf_commit(log->ringbuf, entry);
    
    return HPLOG_OK;
}

/* ============================================================================
 * Log Write with Timestamp (PROVIDED)
 * ============================================================================ */
hplog_result_t hplog_write_ts(hplog_t* log, hplog_level_t level,
                              uint64_t timestamp_ns,
                              const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    if (level < log->min_level) {
        va_end(args);
        return HPLOG_OK;
    }
    
    if (atomic_load(&log->shutdown_requested)) {
        va_end(args);
        return HPLOG_ERR_SHUTDOWN;
    }
    
    hplog_entry_t* entry = NULL;

    switch (log->backpressure) {
        case HPLOG_BP_DROP:
            entry = ringbuf_try_reserve(log->ringbuf);
            break;
        case HPLOG_BP_SPIN:
            for (int retries = 0; retries < 100000; retries++) {
                entry = ringbuf_try_reserve(log->ringbuf);
                if (entry) break;
#if defined(__x86_64__) || defined(__i386__)
                __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
                __asm__ volatile("yield" ::: "memory");
#else
                __asm__ volatile("" ::: "memory");
#endif
            }
            break;
        case HPLOG_BP_YIELD:
            for (int retries = 0; retries < 100000; retries++) {
                entry = ringbuf_try_reserve(log->ringbuf);
                if (entry) break;
                sched_yield();
            }
            break;
    }

    if (!entry) {
        atomic_fetch_add(&log->ringbuf->dropped_count, 1);
        va_end(args);
        return HPLOG_ERR_FULL;
    }

    entry->timestamp_ns = timestamp_ns;
    entry->thread_id = get_thread_id();
    entry->level = (uint8_t)level;
    
    int len = vsnprintf(entry->message, HPLOG_MAX_MSG_LEN, fmt, args);
    entry->msg_len = (len > 0 && len < HPLOG_MAX_MSG_LEN) 
                     ? (uint16_t)len : HPLOG_MAX_MSG_LEN - 1;
    
    va_end(args);
    
    ringbuf_commit(log->ringbuf, entry);
    
    return HPLOG_OK;
}

/* ============================================================================
 * Flush (PROVIDED)
 * ============================================================================ */
void hplog_flush(hplog_t* log) {
    if (!log) return;
    
    uint64_t produced = atomic_load(&log->ringbuf->total_produced);
    while (atomic_load(&log->ringbuf->total_consumed) < produced) {
        sched_yield();
    }
    
    if (log->file_map) {
        msync(log->file_map, log->file_offset, MS_SYNC);
    }
    
    atomic_fetch_add(&log->flush_count, 1);
}

/* ============================================================================
 * TASK 6: Get Statistics (5 pts)
 * 
 * Read all statistics atomically and populate the stats structure.
 * ============================================================================ */
void hplog_get_stats(hplog_t* log, hplog_stats_t* stats) {
    if (!log || !stats) return;
    
    stats->total_produced = atomic_load_explicit(&log->ringbuf->total_produced, memory_order_relaxed);
    stats->total_consumed = atomic_load_explicit(&log->ringbuf->total_consumed, memory_order_relaxed);
    stats->dropped_count  = atomic_load_explicit(&log->ringbuf->dropped_count, memory_order_relaxed);
    stats->bytes_written  = atomic_load_explicit(&log->bytes_written, memory_order_relaxed);
    stats->flush_count    = atomic_load_explicit(&log->flush_count, memory_order_relaxed);

    stats->buffer_usage = (stats->total_produced >= stats->total_consumed)
                         ? stats->total_produced - stats->total_consumed
                         : 0;
}