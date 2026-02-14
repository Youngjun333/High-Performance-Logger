# hplog - High-Performance Lock-Free Logging Library

A high-performance, lock-free logging library in C11 for low-latency,
multi-producer systems. Designed for applications where logging overhead
must be minimized and predictable.

## Features

- Lock-free ring buffer with atomic CAS slot reservation
- Multi-producer, single-consumer architecture
- Memory-mapped file output with dynamic 1MB growth
- Configurable backpressure strategies (DROP, SPIN, YIELD)
- Cache-line-aligned structures to prevent false sharing
- Six log levels: TRACE, DEBUG, INFO, WARN, ERROR, FATAL
- printf-style formatting with compile-time format checking

## Building

Requires GCC with C11 support and pthreads.

```bash
make all        # Build libhplog.a and benchmark executable
make test       # Build and run benchmark suite
make clean      # Remove build artifacts and temp log files
```

Debug build with AddressSanitizer and UBSan is available by uncommenting
the debug CFLAGS block in the Makefile.

## Usage

```c
#include "hp_log.h"

// Initialize logger with output file, minimum level, and backpressure strategy
hplog_t *log = hplog_init("/tmp/app.log", HPLOG_INFO, HPLOG_BP_YIELD);

// Write log messages (thread-safe, lock-free)
HPLOG_INFO(log, "Server started on port %d", 8080);
HPLOG_ERROR(log, "Connection failed: %s", strerror(errno));

// Or use the function API directly
hplog_write(log, HPLOG_WARN, "Disk usage at %d%%", usage);

// Query stats
hplog_stats_t stats;
hplog_get_stats(log, &stats);

// Shut down (flushes remaining entries)
hplog_shutdown(log);
```

## API

| Function | Description |
|---|---|
| `hplog_init(filepath, min_level, backpressure)` | Create a logger instance |
| `hplog_shutdown(log)` | Flush and destroy a logger |
| `hplog_write(log, level, fmt, ...)` | Write a log entry |
| `hplog_write_ts(log, level, timestamp_ns, fmt, ...)` | Write with explicit timestamp |
| `hplog_flush(log)` | Force flush pending entries |
| `hplog_get_stats(log, stats)` | Retrieve performance statistics |

## Backpressure Strategies

| Strategy | Behavior |
|---|---|
| `HPLOG_BP_DROP` | Drop the message when the buffer is full |
| `HPLOG_BP_SPIN` | Spin-wait until a slot becomes available |
| `HPLOG_BP_YIELD` | Yield the CPU and retry |

## Architecture

**Producer path:** Threads call `hplog_write()` which uses an atomic CAS to
reserve a ring buffer slot, writes the entry, then commits it via a sequence
number store-release.

**Consumer path:** A background thread polls the ring buffer, dequeues
committed entries, formats them, and writes to a memory-mapped file that
grows dynamically in 1MB chunks.

The ring buffer holds 65,536 entries (64KB slots). Entries are 320 bytes
and aligned to 64-byte cache lines. The head (producer) and tail (consumer)
pointers live on separate cache lines to avoid false sharing.