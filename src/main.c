#include "hp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <sched.h>

/* ============================================================================
 * Benchmark Configuration
 * ============================================================================ */

#define WARMUP_ITERATIONS       10000
#define BENCHMARK_ITERATIONS    1000000
#define LATENCY_SAMPLES         100000
#define NUM_PRODUCER_THREADS    8
#define SUSTAINED_DURATION_SEC  10

/* ============================================================================
 * Latency Histogram
 * ============================================================================ */

typedef struct {
    uint64_t samples[LATENCY_SAMPLES];
    size_t count;
    pthread_mutex_t lock;
} latency_hist_t;

static void hist_init(latency_hist_t* h) {
    memset(h->samples, 0, sizeof(h->samples));
    h->count = 0;
    pthread_mutex_init(&h->lock, NULL);
}

static void hist_add(latency_hist_t* h, uint64_t latency_ns) {
    pthread_mutex_lock(&h->lock);
    if (h->count < LATENCY_SAMPLES) {
        h->samples[h->count++] = latency_ns;
    }
    pthread_mutex_unlock(&h->lock);
}

static int compare_uint64(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

static void hist_report(latency_hist_t* h, const char* name) {
    if (h->count == 0) {
        printf("  %s: No samples\n", name);
        return;
    }
    
    /* Sort samples for percentile calculation */
    qsort(h->samples, h->count, sizeof(uint64_t), compare_uint64);
    
    uint64_t min = h->samples[0];
    uint64_t max = h->samples[h->count - 1];
    uint64_t p50 = h->samples[h->count / 2];
    uint64_t p90 = h->samples[(h->count * 90) / 100];
    uint64_t p99 = h->samples[(h->count * 99) / 100];
    uint64_t p999 = h->samples[(h->count * 999) / 1000];
    
    /* Calculate mean */
    uint64_t sum = 0;
    for (size_t i = 0; i < h->count; i++) {
        sum += h->samples[i];
    }
    double mean = (double)sum / h->count;
    
    /* Calculate stddev */
    double variance = 0;
    for (size_t i = 0; i < h->count; i++) {
        double diff = (double)h->samples[i] - mean;
        variance += diff * diff;
    }
    double stddev = sqrt(variance / h->count);
    
    printf("  %s Latency (ns):\n", name);
    printf("    Min:     %10lu\n", min);
    printf("    Mean:    %10.1f (stddev: %.1f)\n", mean, stddev);
    printf("    p50:     %10lu\n", p50);
    printf("    p90:     %10lu\n", p90);
    printf("    p99:     %10lu\n", p99);
    printf("    p99.9:   %10lu\n", p999);
    printf("    Max:     %10lu\n", max);
}

/* ============================================================================
 * Benchmark Helpers
 * ============================================================================ */

static void print_separator(void) {
    printf("\n");
    printf("════════════════════════════════════════════════════════════════\n");
}

static void print_header(const char* name) {
    print_separator();
    printf("  %s\n", name);
    print_separator();
}

static double ns_to_mops(uint64_t total_ns, uint64_t operations) {
    return (double)operations / ((double)total_ns / 1e9) / 1e6;
}

/* ============================================================================
 * Benchmark 1: Single-Threaded Throughput
 * ============================================================================ */

static void benchmark_single_thread(void) {
    print_header("Benchmark 1: Single-Threaded Throughput");
    
    hplog_t* log = hplog_init("logs/bench_single.log", HPLOG_INFO, HPLOG_BP_DROP);
    if (!log) {
        fprintf(stderr, "Failed to initialize logger\n");
        return;
    }
    
    /* Warmup */
    printf("  Warming up (%d iterations)...\n", WARMUP_ITERATIONS);
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        HPLOG_INFO(log, "Warmup message %d", i);
    }
    hplog_flush(log);
    
    /* Benchmark */
    printf("  Running benchmark (%d iterations)...\n", BENCHMARK_ITERATIONS);
    
    uint64_t start = hplog_monotonic_ns();
    
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        HPLOG_INFO(log, "Benchmark message %d with some additional data: value=%d", i, i * 2);
    }
    
    uint64_t end = hplog_monotonic_ns();
    
    hplog_flush(log);
    
    hplog_stats_t stats;
    hplog_get_stats(log, &stats);
    
    uint64_t elapsed_ns = end - start;
    double throughput = ns_to_mops(elapsed_ns, BENCHMARK_ITERATIONS);
    double ns_per_op = (double)elapsed_ns / BENCHMARK_ITERATIONS;
    
    printf("\n  Results:\n");
    printf("    Total time:       %.3f ms\n", elapsed_ns / 1e6);
    printf("    Throughput:       %.2f M ops/sec\n", throughput);
    printf("    Latency (mean):   %.1f ns/op\n", ns_per_op);
    printf("    Messages logged:  %lu\n", stats.total_consumed);
    printf("    Messages dropped: %lu\n", stats.dropped_count);
    printf("    Bytes written:    %.2f MB\n", stats.bytes_written / 1e6);
    
    hplog_shutdown(log);
}

/* ============================================================================
 * Benchmark 2: Multi-Producer Throughput
 * ============================================================================ */

typedef struct {
    hplog_t* log;
    int thread_id;
    int iterations;
    uint64_t elapsed_ns;
    uint64_t dropped;
} producer_args_t;

static void* producer_thread(void* arg) {
    producer_args_t* args = (producer_args_t*)arg;
    
    uint64_t start = hplog_monotonic_ns();
    uint64_t dropped = 0;
    
    for (int i = 0; i < args->iterations; i++) {
        hplog_result_t result = HPLOG_INFO(args->log, 
            "Thread %d message %d: data=%d", args->thread_id, i, i * args->thread_id);
        if (result != HPLOG_OK) {
            dropped++;
        }
    }
    
    args->elapsed_ns = hplog_monotonic_ns() - start;
    args->dropped = dropped;
    
    return NULL;
}

static void benchmark_multi_producer(void) {
    print_header("Benchmark 2: Multi-Producer Throughput");
    
    hplog_t* log = hplog_init("logs/bench_multi.log", HPLOG_INFO, HPLOG_BP_DROP);
    if (!log) {
        fprintf(stderr, "Failed to initialize logger\n");
        return;
    }
    
    pthread_t threads[NUM_PRODUCER_THREADS];
    producer_args_t args[NUM_PRODUCER_THREADS];
    int iterations_per_thread = BENCHMARK_ITERATIONS / NUM_PRODUCER_THREADS;
    
    printf("  Running with %d producer threads...\n", NUM_PRODUCER_THREADS);
    printf("  Each thread: %d iterations\n", iterations_per_thread);
    
    uint64_t start = hplog_monotonic_ns();
    
    /* Start producer threads */
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        args[i].log = log;
        args[i].thread_id = i;
        args[i].iterations = iterations_per_thread;
        args[i].elapsed_ns = 0;
        args[i].dropped = 0;
        pthread_create(&threads[i], NULL, producer_thread, &args[i]);
    }
    
    /* Wait for all threads */
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    uint64_t end = hplog_monotonic_ns();
    
    hplog_flush(log);
    
    hplog_stats_t stats;
    hplog_get_stats(log, &stats);
    
    uint64_t total_elapsed = end - start;
    uint64_t total_iterations = iterations_per_thread * NUM_PRODUCER_THREADS;
    double throughput = ns_to_mops(total_elapsed, total_iterations);
    
    uint64_t total_dropped = 0;
    uint64_t max_thread_time = 0;
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        total_dropped += args[i].dropped;
        if (args[i].elapsed_ns > max_thread_time) {
            max_thread_time = args[i].elapsed_ns;
        }
    }
    
    printf("\n  Results:\n");
    printf("    Total time:         %.3f ms\n", total_elapsed / 1e6);
    printf("    Aggregate throughput: %.2f M ops/sec\n", throughput);
    printf("    Per-thread throughput: %.2f M ops/sec\n", throughput / NUM_PRODUCER_THREADS);
    printf("    Total iterations:   %lu\n", total_iterations);
    printf("    Messages logged:    %lu\n", stats.total_consumed);
    printf("    Messages dropped:   %lu (%.2f%%)\n", 
           stats.dropped_count, 
           (double)stats.dropped_count / total_iterations * 100);
    printf("    Bytes written:      %.2f MB\n", stats.bytes_written / 1e6);
    
    /* Per-thread stats */
    printf("\n  Per-thread stats:\n");
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        printf("    Thread %d: %.3f ms, %lu dropped\n", 
               i, args[i].elapsed_ns / 1e6, args[i].dropped);
    }
    
    hplog_shutdown(log);
}

/* ============================================================================
 * Benchmark 3: Latency Distribution
 * ============================================================================ */

static void benchmark_latency(void) {
    print_header("Benchmark 3: Latency Distribution");
    
    hplog_t* log = hplog_init("logs/bench_latency.log", HPLOG_INFO, HPLOG_BP_DROP);
    if (!log) {
        fprintf(stderr, "Failed to initialize logger\n");
        return;
    }
    
    latency_hist_t hist;
    hist_init(&hist);
    
    /* Warmup */
    printf("  Warming up...\n");
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        HPLOG_INFO(log, "Warmup %d", i);
    }
    hplog_flush(log);
    
    /* Collect latency samples */
    printf("  Collecting %d latency samples...\n", LATENCY_SAMPLES);
    
    for (int i = 0; i < LATENCY_SAMPLES; i++) {
        uint64_t start = hplog_monotonic_ns();
        HPLOG_INFO(log, "Latency test message %d", i);
        uint64_t end = hplog_monotonic_ns();
        
        hist_add(&hist, end - start);
        
        /* Small delay to avoid sustained backpressure */
        if (i % 1000 == 0) {
            struct timespec ts = {0, 100000}; /* 100us */
            nanosleep(&ts, NULL);
        }
    }
    
    hplog_flush(log);
    
    printf("\n  Results:\n");
    hist_report(&hist, "Log Write");
    
    hplog_stats_t stats;
    hplog_get_stats(log, &stats);
    printf("\n    Messages dropped: %lu (%.2f%%)\n", 
           stats.dropped_count,
           (double)stats.dropped_count / LATENCY_SAMPLES * 100);
    
    hplog_shutdown(log);
}

/* ============================================================================
 * Benchmark 4: Backpressure Comparison
 * ============================================================================ */

static void benchmark_backpressure(void) {
    print_header("Benchmark 4: Backpressure Strategy Comparison");
    
    const char* strategy_names[] = {"DROP", "SPIN", "YIELD"};
    hplog_backpressure_t strategies[] = {HPLOG_BP_DROP, HPLOG_BP_SPIN, HPLOG_BP_YIELD};
    int num_strategies = 3;
    
    for (int s = 0; s < num_strategies; s++) {
        printf("\n  Testing %s strategy...\n", strategy_names[s]);
        
        char filename[64];
        snprintf(filename, sizeof(filename), "logs/bench_bp_%s.log", strategy_names[s]);
        
        hplog_t* log = hplog_init(filename, HPLOG_INFO, strategies[s]);
        if (!log) {
            fprintf(stderr, "Failed to initialize logger\n");
            continue;
        }
        
        /* Burst test - try to overwhelm the consumer */
        int burst_size = HPLOG_BUFFER_SIZE * 2;  /* 2x buffer size */
        
        uint64_t start = hplog_monotonic_ns();
        
        for (int i = 0; i < burst_size; i++) {
            HPLOG_INFO(log, "Burst message %d with data %d", i, i * 3);
        }
        
        uint64_t end = hplog_monotonic_ns();
        
        hplog_flush(log);
        
        hplog_stats_t stats;
        hplog_get_stats(log, &stats);
        
        double throughput = ns_to_mops(end - start, burst_size);
        
        printf("    Burst size:       %d messages\n", burst_size);
        printf("    Time:             %.3f ms\n", (end - start) / 1e6);
        printf("    Throughput:       %.2f M ops/sec\n", throughput);
        printf("    Messages logged:  %lu\n", stats.total_consumed);
        printf("    Messages dropped: %lu (%.2f%%)\n", 
               stats.dropped_count,
               (double)stats.dropped_count / burst_size * 100);
        
        hplog_shutdown(log);
    }
}

/* ============================================================================
 * Benchmark 5: Sustained Load
 * ============================================================================ */

typedef struct {
    hplog_t* log;
    _Atomic bool* running;
    uint64_t count;
    uint64_t dropped;
} sustained_args_t;

static void* sustained_producer(void* arg) {
    sustained_args_t* args = (sustained_args_t*)arg;
    uint64_t count = 0;
    uint64_t dropped = 0;
    
    while (atomic_load(args->running)) {
        hplog_result_t result = HPLOG_INFO(args->log, 
            "Sustained load message %lu", count);
        if (result != HPLOG_OK) {
            dropped++;
        }
        count++;
    }
    
    args->count = count;
    args->dropped = dropped;
    return NULL;
}

static void benchmark_sustained(void) {
    print_header("Benchmark 5: Sustained Load Test");
    
    printf("  Duration: %d seconds\n", SUSTAINED_DURATION_SEC);
    printf("  Producers: %d threads\n", NUM_PRODUCER_THREADS);
    
    hplog_t* log = hplog_init("logs/bench_sustained.log", HPLOG_INFO, HPLOG_BP_DROP);
    if (!log) {
        fprintf(stderr, "Failed to initialize logger\n");
        return;
    }
    
    _Atomic bool running = true;
    pthread_t threads[NUM_PRODUCER_THREADS];
    sustained_args_t args[NUM_PRODUCER_THREADS];
    
    /* Start producer threads */
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        args[i].log = log;
        args[i].running = &running;
        args[i].count = 0;
        args[i].dropped = 0;
        pthread_create(&threads[i], NULL, sustained_producer, &args[i]);
    }
    
    /* Monitor and report every second */
    printf("\n  Progress:\n");
    for (int sec = 0; sec < SUSTAINED_DURATION_SEC; sec++) {
        sleep(1);
        
        hplog_stats_t stats;
        hplog_get_stats(log, &stats);
        
        double throughput = (double)stats.total_produced / (sec + 1) / 1e6;
        printf("    [%2d/%d] Produced: %10lu, Consumed: %10lu, Dropped: %6lu (%.2f M/s)\n",
               sec + 1, SUSTAINED_DURATION_SEC,
               stats.total_produced, stats.total_consumed, stats.dropped_count,
               throughput);
    }
    
    /* Stop threads */
    atomic_store(&running, false);
    
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    hplog_flush(log);
    
    hplog_stats_t stats;
    hplog_get_stats(log, &stats);
    
    uint64_t total_attempts = 0;
    uint64_t total_dropped = 0;
    for (int i = 0; i < NUM_PRODUCER_THREADS; i++) {
        total_attempts += args[i].count;
        total_dropped += args[i].dropped;
    }
    
    double avg_throughput = (double)stats.total_produced / SUSTAINED_DURATION_SEC / 1e6;
    
    printf("\n  Final Results:\n");
    printf("    Total produced:   %lu\n", stats.total_produced);
    printf("    Total consumed:   %lu\n", stats.total_consumed);
    printf("    Total dropped:    %lu (%.4f%%)\n", 
           stats.dropped_count,
           (double)stats.dropped_count / total_attempts * 100);
    printf("    Avg throughput:   %.2f M ops/sec\n", avg_throughput);
    printf("    Bytes written:    %.2f MB (%.2f MB/s)\n", 
           stats.bytes_written / 1e6,
           stats.bytes_written / 1e6 / SUSTAINED_DURATION_SEC);
    printf("    Buffer flushes:   %lu\n", stats.flush_count);
    
    hplog_shutdown(log);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char* argv[]) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║     HIGH-PERFORMANCE LOGGING SYSTEM - BENCHMARK SUITE            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    
    printf("\nConfiguration:\n");
    printf("  Ring buffer size:  %d entries\n", HPLOG_BUFFER_SIZE);
    printf("  Entry size:        %zu bytes\n", sizeof(hplog_entry_t));
    printf("  Max message len:   %d bytes\n", HPLOG_MAX_MSG_LEN);
    printf("  Benchmark iters:   %d\n", BENCHMARK_ITERATIONS);
    printf("  Producer threads:  %d\n", NUM_PRODUCER_THREADS);
    
    /* Run benchmarks */
    benchmark_single_thread();
    benchmark_multi_producer();
    benchmark_latency();
    benchmark_backpressure();
    benchmark_sustained();
    
    print_separator();
    printf("  All benchmarks complete!\n");
    print_separator();
    printf("\n");
    
    return 0;
}