/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HIMAX_V4L2_H
#define HIMAX_V4L2_H

#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <drm/drm_panel.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>
#include "hx-raw-policy.h"
#include "hx-v4l2-uapi.h"

#define HIMAX_MAX_RX 60U
#define HIMAX_MAX_TX 40U
#define HIMAX_FULL_EVENT_STACK_SIZE HX_RAW_PAYLOAD_SIZE
#define HIMAX_BUS_READ_HEADER_SIZE 3U
#define HIMAX_EVENT_STACK_ADDRESS 0x30
#define HIMAX_SPI_CS_SETUP_TIME 300U
#define HIMAX_HX83121A_FLASH_SIZE (255 * 1024)

struct himax_ts_data {
	struct device *dev;
	struct spi_device *spi;
	struct gpio_desc *gpiod_rst;
	u8 *xfer_buf;
	u8 *event_buf;
	u32 spi_xfer_max_sz;
	u32 xfer_buf_sz;
	u32 event_buf_sz;
	spinlock_t irq_lock;
	struct mutex op_lock;
	struct mutex video_lock;
	spinlock_t buffer_lock;
	struct list_head buffers;
	struct v4l2_device v4l2;
	struct video_device *video;
	struct vb2_queue queue;
	struct drm_panel_follower panel_follower;
	struct delayed_work panel_reinit_work;
	struct delayed_work charger_work;
	struct notifier_block charger_notifier;
	bool irq_enabled;
	struct hx_raw_power_state power;
	struct hx_raw_charger_state charger;
	bool otp_tried;
	u8 afe_command_slot;
	u8 read_errors;
	u64 epoch;
	u64 sequence;
	u64 last_command_ns[HX_RAW_RESET + 1];
	u32 frame_flags;
	u32 coordinate_flags;
	char project_id[16];
};

void himax_int_enable(struct himax_ts_data *ts, bool enable);
void himax_lock(struct himax_ts_data *ts);
void himax_unlock(struct himax_ts_data *ts);
int himax_spi_read(struct himax_ts_data *ts, u8 command, u8 *buf, u32 len);
int himax_verify_running_ic(struct himax_ts_data *ts);
int himax_mcu_check_crc(struct himax_ts_data *ts, u32 addr, int len, u32 *crc);
int hx83121a_chip_detect(struct himax_ts_data *ts);
int himax_get_project_id_otp(struct himax_ts_data *ts, char *id, size_t len);
int himax_enable_fw_reload(struct himax_ts_data *ts);
int himax_mcu_power_on_init(struct himax_ts_data *ts);
int himax_restore_raw_runtime(struct himax_ts_data *ts);
int himax_afe_command(struct himax_ts_data *ts, u32 action, u32 parameter);
int himax_set_charger(struct himax_ts_data *ts, bool connected);

#endif
