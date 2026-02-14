#ifndef HPLOG_H
#define HPLOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define HPLOG_BUFFER_SIZE       (1 << 16)  /* 65536 entries - must be power of 2 */
#define HPLOG_BUFFER_MASK       (HPLOG_BUFFER_SIZE - 1)
#define HPLOG_MAX_MSG_LEN       256
#define HPLOG_FILE_INITIAL_SIZE (1 << 20)  /* 1 MB */
#define HPLOG_FILE_GROW_SIZE    (1 << 20)  /* 1 MB */
#define HPLOG_CONSUMER_POLL_NS  1000       /* 1 microsecond */

/* ============================================================================
 * Log Levels
 * ============================================================================ */

typedef enum {
    HPLOG_TRACE = 0,
    HPLOG_DEBUG = 1,
    HPLOG_INFO  = 2,
    HPLOG_WARN  = 3,
    HPLOG_ERROR = 4,
    HPLOG_FATAL = 5,
    HPLOG_LEVEL_COUNT
} hplog_level_t;

/* ============================================================================
 * Backpressure Strategies
 * ============================================================================ */

typedef enum {
    HPLOG_BP_DROP,      /* Drop message when buffer full */
    HPLOG_BP_SPIN,      /* Spin-wait until space available */
    HPLOG_BP_YIELD      /* Yield CPU and retry */
} hplog_backpressure_t;

/* ============================================================================
 * Return Codes
 * ============================================================================ */

typedef enum {
    HPLOG_OK = 0,
    HPLOG_ERR_INIT = -1,
    HPLOG_ERR_FULL = -2,
    HPLOG_ERR_MMAP = -3,
    HPLOG_ERR_FILE = -4,
    HPLOG_ERR_THREAD = -5,
    HPLOG_ERR_SHUTDOWN = -6
} hplog_result_t;

/* ============================================================================
 * Log Entry Structure
 * 
 * Cache-line aligned (64 bytes) for performance.
 * Fields ordered largest-first to minimize padding.
 * ============================================================================ */

typedef struct __attribute__((aligned(64))) {
    uint64_t            timestamp_ns;   /* 8 bytes @ 0  */
    _Atomic uint32_t    sequence;       /* 4 bytes @ 8  */
    uint32_t            thread_id;      /* 4 bytes @ 12 */
    uint16_t            msg_len;        /* 2 bytes @ 16 */
    uint8_t             level;          /* 1 byte  @ 18 */
    char                message[HPLOG_MAX_MSG_LEN];  /* 256 bytes @ 19 */
} hplog_entry_t;

/* ============================================================================
 * Ring Buffer Structure
 * 
 * Producer (head) and consumer (tail) on separate cache lines to avoid
 * false sharing between threads.
 * ============================================================================ */

typedef struct {
    hplog_entry_t           entries[HPLOG_BUFFER_SIZE];
    
    _Atomic uint64_t   head __attribute__((aligned(64)));  /* Producer */
    _Atomic uint64_t   tail __attribute__((aligned(64)));  /* Consumer */
    
    /* Statistics */
    _Atomic uint64_t   total_produced __attribute__((aligned(64)));
    _Atomic uint64_t   total_consumed;
    _Atomic uint64_t   dropped_count;
} hplog_ringbuf_t;

/* ============================================================================
 * Logger Context
 * ============================================================================ */

typedef struct {
    hplog_ringbuf_t*        ringbuf;
    
    /* mmap'd output file */
    int                     fd;
    char*                   file_map;
    size_t                  file_size;
    size_t                  file_offset;
    char                    filepath[256];
    
    /* Consumer thread */
    pthread_t               consumer_thread;
    atomic_bool             running;
    atomic_bool             shutdown_requested;
    
    /* Configuration */
    hplog_level_t           min_level;
    hplog_backpressure_t    backpressure;
    bool                    sync_on_flush;
    
    /* Performance stats */
    _Atomic uint_fast64_t   bytes_written;
    _Atomic uint_fast64_t   flush_count;
} hplog_t;

/* ============================================================================
 * Statistics Structure
 * ============================================================================ */

typedef struct {
    uint64_t total_produced;
    uint64_t total_consumed;
    uint64_t dropped_count;
    uint64_t bytes_written;
    uint64_t flush_count;
    uint64_t buffer_usage;
} hplog_stats_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

hplog_t* hplog_init(const char* filepath, 
                    hplog_level_t min_level,
                    hplog_backpressure_t backpressure);

void hplog_shutdown(hplog_t* log);

hplog_result_t hplog_write(hplog_t* log, hplog_level_t level, 
                           const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

hplog_result_t hplog_write_ts(hplog_t* log, hplog_level_t level,
                              uint64_t timestamp_ns,
                              const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

void hplog_flush(hplog_t* log);

void hplog_get_stats(hplog_t* log, hplog_stats_t* stats);

/* ============================================================================
 * Timestamp Helpers
 * ============================================================================ */

static inline uint64_t hplog_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t hplog_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#define HPLOG_TRACE(log, fmt, ...) \
    hplog_write(log, HPLOG_TRACE, fmt, ##__VA_ARGS__)

#define HPLOG_DEBUG(log, fmt, ...) \
    hplog_write(log, HPLOG_DEBUG, fmt, ##__VA_ARGS__)

#define HPLOG_INFO(log, fmt, ...) \
    hplog_write(log, HPLOG_INFO, fmt, ##__VA_ARGS__)

#define HPLOG_WARN(log, fmt, ...) \
    hplog_write(log, HPLOG_WARN, fmt, ##__VA_ARGS__)

#define HPLOG_ERROR(log, fmt, ...) \
    hplog_write(log, HPLOG_ERROR, fmt, ##__VA_ARGS__)

#define HPLOG_FATAL(log, fmt, ...) \
    hplog_write(log, HPLOG_FATAL, fmt, ##__VA_ARGS__)

#endif /* HPLOG_H */