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

static const char* LEVEL_STRINGS[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

static __thread uint32_t tls_thread_id = 0;
static _Atomic uint32_t next_thread_id = 1;

static inline uint32_t get_thread_id(void) {
    if (tls_thread_id == 0) {
        tls_thread_id = atomic_fetch_add(&next_thread_id, 1);
    }
    return tls_thread_id;
}

/**
 * ringbuf_init - Initializes the ring buffer
 * @rb: ring buffer context 
 *
 * Initializes the ring buffer
 * 
 * Return: None 
 */
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

static hplog_entry_t* ringbuf_try_reserve(hplog_ringbuf_t* rb) {
    while (1) {
        uint64_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
        hplog_entry_t* entry = &rb->entries[head & HPLOG_BUFFER_MASK];
        uint32_t seq = atomic_load_explicit(&entry->sequence, memory_order_acquire);

        if (seq == (uint32_t)head) {
            if (atomic_compare_exchange_weak_explicit(&rb->head, &head, head + 1,
                    memory_order_acq_rel, memory_order_relaxed)) {
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

static void ringbuf_commit(hplog_ringbuf_t* rb, hplog_entry_t* entry) {
    uint32_t i = (uint32_t)(entry - rb->entries);
    atomic_store_explicit(&entry->sequence, i + 1, memory_order_release);
    atomic_fetch_add_explicit(&rb->total_produced, 1, memory_order_release);
}

static hplog_entry_t* ringbuf_try_dequeue(hplog_ringbuf_t* rb) {
    uint64_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    hplog_entry_t *entry = &rb->entries[tail & HPLOG_BUFFER_MASK];
    uint32_t seq = atomic_load_explicit(&entry->sequence, memory_order_acquire);
    
    if (seq == (uint32_t)(tail & HPLOG_BUFFER_MASK) + 1) {
        return entry;
    }

    return NULL;
}

static void ringbuf_release(hplog_ringbuf_t* rb, hplog_entry_t* entry) {
    uint64_t tail = atomic_fetch_add_explicit(&rb->tail, 1, memory_order_relaxed);
    uint32_t seq = (uint32_t)(tail + HPLOG_BUFFER_SIZE);
    atomic_store_explicit(&entry->sequence, seq, memory_order_release);
    atomic_fetch_add_explicit(&rb->total_consumed, 1, memory_order_release);
}

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

void hplog_shutdown(hplog_t* log) {
    if (!log) return;
    
    atomic_store_explicit(&log->shutdown_requested, true, memory_order_release);

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

/**
 * holog_get_stats - Retrieves the statistics for the logger 
 * @log: logger context
 * @stats: stats context
 * 
 * Fills out the statistics. 
 * 
 * Return: None 
 */
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