CC = gcc
AR = ar

CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O3 -march=native
CFLAGS += -D_GNU_SOURCE -fPIC -Iinclude

# Uncomment for debugging (disables optimizations, enables sanitizers)
# CFLAGS = -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE -Iinclude
# CFLAGS += -fsanitize=address -fsanitize=undefined

LDFLAGS = -pthread -lm -lrt

BIN_DIR = bin
LOG_DIR = logs
LIB_NAME = $(BIN_DIR)/libhplog.a
BENCHMARK = $(BIN_DIR)/benchmark

.PHONY: all clean test submit

all: $(LIB_NAME) $(BENCHMARK)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LOG_DIR):
	mkdir -p $(LOG_DIR)

$(LIB_NAME): $(BIN_DIR)/hp_log.o | $(BIN_DIR)
	$(AR) rcs $@ $^

$(BIN_DIR)/hp_log.o: src/hp_log.c include/hp_log.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BENCHMARK): src/main.c $(LIB_NAME) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< -L$(BIN_DIR) -lhplog $(LDFLAGS)

test: $(BENCHMARK) | $(LOG_DIR)
	@echo "Running benchmark suite..."
	@echo "=========================="
	./$(BENCHMARK)

clean:
	rm -rf $(BIN_DIR) $(LOG_DIR)
