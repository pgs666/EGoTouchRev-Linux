CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror

.PHONY: all clean test

all: hx-algo-test

ALGO_DIR := ../touchscreen-hx83121a-dkms
ALGO_SOURCES := $(ALGO_DIR)/hx-algo.c $(ALGO_DIR)/hx-algo-preprocess.c \
	$(ALGO_DIR)/hx-baseline-state.c \
	$(ALGO_DIR)/hx-safe-baseline.c \
	$(ALGO_DIR)/hx-algo-detect.c $(ALGO_DIR)/hx-algo-resolve.c \
	$(ALGO_DIR)/hx-algo-track.c
TEST_SOURCES := hx-algo-test.c hx-algo-detection-test.c \
	hx-algo-runtime-test.c hx-algo-tracking-test.c

hx-algo-test: $(TEST_SOURCES) hx-algo-test-common.h $(ALGO_SOURCES) \
		$(ALGO_DIR)/hx-algo.h $(ALGO_DIR)/hx-algo-internal.h host-compat.h
	$(CC) $(CFLAGS) -DHX_ALGO_HOST_TEST \
		-DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS=1 \
		-I$(ALGO_DIR) $(ALGO_SOURCES) $(TEST_SOURCES) -o $@

test: hx-algo-test
	./hx-algo-test

clean:
	rm -f hx-algo-test
