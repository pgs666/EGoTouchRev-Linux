// SPDX-License-Identifier: MIT
#pragma once

/* Single-transfer SPI shim. Compile the actual native functions unchanged;
 * only kernel allocation/device/logging and the physical spi_sync are replaced.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif

#ifdef __cplusplus
extern "C" {
#endif
typedef uint8_t u8;
typedef uint32_t u32;
struct hx_spi_test_bus {
    void *context;
    int (*transfer)(void *, const void *, void *, u32, u32 *);
};
struct spi_device { struct hx_spi_test_bus *bus; };
struct spi_transfer {
    unsigned int len;
    const void *tx_buf;
    void *rx_buf;
    unsigned int cs_change;
};
struct spi_message {
    struct spi_transfer *transfer;
    unsigned int actual_length;
};
struct himax_ts_data {
    struct spi_device *spi;
    void *dev;
    u8 *xfer_buf;
    u32 xfer_buf_sz;
    u8 *event_buf;
    u32 event_buf_sz;
};
static inline void spi_message_init(struct spi_message *message) {
    memset(message, 0, sizeof(*message));
}
static inline void spi_message_add_tail(struct spi_transfer *transfer,
                                        struct spi_message *message) {
    message->transfer = transfer;
}
static inline int spi_sync(struct spi_device *device, struct spi_message *message) {
    struct spi_transfer *transfer = message->transfer;
    message->actual_length = 0;
    return device->bus->transfer(device->bus->context, transfer->tx_buf,
                                transfer->rx_buf, transfer->len, &message->actual_length);
}
#define dev_err_ratelimited(...) ((void)0)
#define dev_err(...) ((void)0)
int himax_spi_read(struct himax_ts_data *, u8, u8 *, u32);
int hx_spi_native_write(struct himax_ts_data *, u8, const u8 *, const u8 *, u32);
int hx_spi_windows_read(struct hx_spi_test_bus *, u8, u8 *, u32);
int hx_spi_windows_write(struct hx_spi_test_bus *, u8, const u8 *, const u8 *, u32);
int hx_spi_native_read_event(struct himax_ts_data *);
#ifdef __cplusplus
}
#endif
