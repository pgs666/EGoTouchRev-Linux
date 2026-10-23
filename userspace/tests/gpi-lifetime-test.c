// SPDX-License-Identifier: GPL-2.0-only
/* Exact GPI function bodies with host allocation/lock/hardware shims.
 * Tests allocation ownership, not DMA execution or IRQ synchronization.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum gpi_pm_state { DISABLE_STATE, PREPARE_TERMINATE, ACTIVE_STATE };
enum { MAX_CHANNELS_PER_GPII = 2, GPI_CH_CMD_RESET, GPI_CH_CMD_DE_ALLOC,
       GPI_EV_CMD_DEALLOC, GFP_NOWAIT };
struct dma_chan { unsigned int unused; };
struct dma_slave_config { void *peripheral_config; size_t peripheral_size; };
struct gpi_ring { bool configured; };
struct gpii;
struct gchan {
    struct dma_chan chan;
    struct gpii *gpii;
    enum gpi_pm_state pm_state;
    struct gpi_ring ch_ring;
    int vc;
    void *config;
};
struct gpii {
    struct gchan gchan[MAX_CHANNELS_PER_GPII];
    int ctrl_lock, pm_lock, ev_task;
    enum gpi_pm_state pm_state;
    struct gpi_ring ev_ring;
};
static struct gchan *to_gchan(struct dma_chan *chan) { return (struct gchan *)chan; }
static bool fail_allocation;
static void *krealloc(void *pointer, size_t length, int flags) {
    (void)flags;
    return fail_allocation ? NULL : realloc(pointer, length);
}
static int gpi_send_cmd(struct gpii *instance, struct gchan *channel, int command) {
    (void)instance;
    (void)channel;
    (void)command;
    return 0;
}
static void gpi_free_ring(struct gpi_ring *ring, struct gpii *instance) {
    (void)instance;
    ring->configured = false;
}
#define kfree free
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define write_lock_irq(p) ((void)(p))
#define write_unlock_irq(p) ((void)(p))
#define dev_err(...) ((void)0)
#define gpi_stop_chan(c) ((void)(c))
#define gpi_reset_chan(c, cmd) ((void)(c), (void)(cmd))
#define vchan_free_chan_resources(v) ((void)(v))
#define tasklet_kill(t) ((void)(t))
#define gpi_disable_interrupts(i) ((void)(i))
#include "gpi-source.inc"

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(2);
    }
}

int main(int argc, char **argv) {
    check(argc == 2, "mode required");
    struct gpii instance = {0};
    uint8_t payload[64] = {1, 0, 1, 1, 1, 4, 0, 0};
    uint64_t prefix = 0;
    for (int i = 7; i >= 0; --i) prefix = prefix * 256 + payload[i];
    check(prefix == UINT64_C(0x0000040101010001), "captured bad pointer matches SPI prefix");
    struct dma_slave_config config = {payload, 24};
    for (int i = 0; i < MAX_CHANNELS_PER_GPII; ++i)
        instance.gchan[i].gpii = &instance;
    struct gchan *channel = &instance.gchan[0];
    check(!gpi_peripheral_config(&channel->chan, &config), "initial configuration");
    if (!strcmp(argv[1], "retained")) {
        for (int i = 0; i < 100; ++i)
            check(!gpi_peripheral_config(&channel->chan, &config), "live reconfiguration");
        gpi_free_chan_resources(&channel->chan);
        puts("retained channel control passed");
        return 0;
    }
    gpi_free_chan_resources(&channel->chan);
    /* Original source must trip ASan here; patched source must allocate a new
     * object, even if the host allocator later chooses the same address.
     */
    check(!gpi_peripheral_config(&channel->chan, &config), "configuration after release");
    gpi_free_chan_resources(&channel->chan);
    check(channel->config == NULL, "patched release clears ownership");

    for (int round = 0; round < 100; ++round) {
        instance.pm_state = ACTIVE_STATE;
        for (int i = 0; i < MAX_CHANNELS_PER_GPII; ++i) {
            channel = &instance.gchan[i];
            channel->pm_state = ACTIVE_STATE;
            channel->ch_ring.configured = true;
            config.peripheral_size = 24 + (round % 3) * 16;
            check(!gpi_peripheral_config(&channel->chan, &config), "reacquired channel configured");
            check(!memcmp(channel->config, payload, config.peripheral_size), "configuration bytes");
        }
        channel = &instance.gchan[0];
        void *previous = channel->config;
        fail_allocation = true;
        check(gpi_peripheral_config(&channel->chan, &config) == -ENOMEM &&
              channel->config == previous, "failed resize preserves live allocation");
        fail_allocation = false;
        struct dma_slave_config invalid = {0};
        check(gpi_peripheral_config(&channel->chan, &invalid) == -EINVAL &&
              channel->config == previous, "invalid configuration preserves live allocation");
        for (int i = 0; i < MAX_CHANNELS_PER_GPII; ++i) {
            channel = &instance.gchan[i];
            gpi_free_chan_resources(&channel->chan);
            check(channel->config == NULL, "each released channel clears ownership");
            gpi_free_chan_resources(&channel->chan);
            fail_allocation = true;
            check(gpi_peripheral_config(&channel->chan, &config) == -ENOMEM &&
                  channel->config == NULL, "failed reacquire keeps NULL allocation");
            fail_allocation = false;
        }
    }
    puts("GPI shipped patch: 100 two-channel release/reacquire cycles, varying sizes, "
         "invalid config, allocation failure and repeated release passed");
}
