CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror

.PHONY: all clean test

all: hx-algo-test hx-otp-test hx-raw-policy-test

ALGO_DIR := ../touchscreen-hx83121a-dkms
ALGO_SOURCES := $(ALGO_DIR)/hx-algo.c $(ALGO_DIR)/hx-algo-preprocess.c \
	$(ALGO_DIR)/hx-baseline-state.c \
	$(ALGO_DIR)/hx-baseline-property.c \
	$(ALGO_DIR)/hx-baseline-observe.c \
	$(ALGO_DIR)/hx-baseline-update.c \
	$(ALGO_DIR)/hx-blreset.c \
	$(ALGO_DIR)/hx-blreset-abnormal.c \
	$(ALGO_DIR)/hx-blreset-state.c \
	$(ALGO_DIR)/hx-blrecal.c \
	$(ALGO_DIR)/hx-blsm-shb.c \
	$(ALGO_DIR)/hx-safe-baseline.c \
	$(ALGO_DIR)/hx-safe-baseline-signal.c \
	$(ALGO_DIR)/hx-safe-baseline-stats.c \
	$(ALGO_DIR)/hx-safe-baseline-judge.c \
	$(ALGO_DIR)/hx-safe-baseline-queue.c \
	$(ALGO_DIR)/hx-panel-profile.c \
	$(ALGO_DIR)/hx-algo-detect.c $(ALGO_DIR)/hx-algo-resolve.c \
	$(ALGO_DIR)/hx-algo-track.c
TEST_SOURCES := hx-algo-test.c hx-algo-detection-test.c \
	hx-algo-runtime-test.c hx-algo-tracking-test.c

hx-algo-test: $(TEST_SOURCES) hx-algo-test-common.h $(ALGO_SOURCES) \
		$(ALGO_DIR)/hx-algo.h $(ALGO_DIR)/hx-algo-internal.h host-compat.h
	$(CC) $(CFLAGS) -DHX_ALGO_HOST_TEST \
		-DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS=1 \
		-DCONFIG_TOUCHSCREEN_HIMAX_HX83121A_EXPERIMENTAL_SAFE_BASELINE=1 \
		-I$(ALGO_DIR) $(ALGO_SOURCES) $(TEST_SOURCES) -o $@

hx-otp-test: hx-otp-test.c $(ALGO_DIR)/hx-otp.c $(ALGO_DIR)/hx-otp.h
	$(CC) $(CFLAGS) -I$(ALGO_DIR) $(ALGO_DIR)/hx-otp.c hx-otp-test.c -o $@

hx-raw-policy-test: hx-raw-policy-test.c $(ALGO_DIR)/hx-raw-policy.h
	$(CC) $(CFLAGS) -I$(ALGO_DIR) hx-raw-policy-test.c -o $@

test: hx-algo-test hx-otp-test hx-raw-policy-test
	./hx-algo-test
	./hx-otp-test
	./hx-raw-policy-test

clean:
	rm -f hx-algo-test hx-otp-test hx-raw-policy-test
