// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

enum { DMA_TRANS_NOERROR, DMA_TRANS_ABORTED };
struct dmaengine_result { int result; unsigned int residue; };
struct spi_message { int status; unsigned int actual_length; };
struct spi_controller { struct spi_message *cur_msg; unsigned int completed; };
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
static void spi_finalize_current_transfer(struct spi_controller *spi) {
    ++spi->completed;
}
#define spi_gsi_callback_result original_completion
#include "original.c"
#undef spi_gsi_callback_result
#include "patched.c"

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

/* Model only the core's status gate and accounting, not the physical SPI
 * engine. Both callbacks above are compiled from the actual patch fixture.
 */
static struct spi_message run(int patched, int error_at, unsigned int residue,
                              unsigned int *completed) {
    struct spi_message message = { .status = -EINPROGRESS };
    struct spi_controller controller = { .cur_msg = &message };
    const unsigned int lengths[] = {3, 7};
    for (int i = 0; i < 2; ++i) {
        struct dmaengine_result result = {
            .result = i == error_at && !residue ? DMA_TRANS_ABORTED : DMA_TRANS_NOERROR,
            .residue = i == error_at ? residue : 0,
        };
        if (patched) spi_gsi_callback_result(&controller, &result);
        else original_completion(&controller, &result);
        if (message.status != -EINPROGRESS) break;
        message.actual_length += lengths[i];
    }
    if (message.status == -EINPROGRESS) message.status = 0;
    *completed = controller.completed;
    return message;
}

int main(void) {
    unsigned int completed;
    struct spi_message message = run(0, -1, 0, &completed);
    check(message.status == 0 && !message.actual_length && completed == 1,
          "old callback reproduces zero-length success and early message exit");
    message = run(1, -1, 0, &completed);
    check(message.status == 0 && message.actual_length == 10 && completed == 2,
          "fixed callback preserves core accounting and subsequent transfers");
    for (int error_at = 0; error_at < 2; ++error_at) {
        for (unsigned int residue = 0; residue <= 3; ++residue) {
            message = run(1, error_at, residue, &completed);
            check(message.status == -EIO && completed == (unsigned int)error_at + 1 &&
                  message.actual_length == (error_at ? 3u : 0u),
                  "DMA error/residue completes once, fails and preserves preceding length");
        }
    }
    message.status = -ESHUTDOWN;
    struct spi_controller controller = { .cur_msg = &message };
    const struct dmaengine_result success = {0};
    spi_gsi_callback_result(&controller, &success);
    check(message.status == -ESHUTDOWN && controller.completed == 1,
          "successful callback cannot erase an existing message error");
    puts("GENI shipped patch: zero-length reproduction, accounting, chained transfers, "
         "DMA errors/residue and preserved errors passed (core status-gate model)");
}
