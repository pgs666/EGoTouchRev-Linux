/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef HX_V4L2_UAPI_H
#define HX_V4L2_UAPI_H

#include <linux/types.h>
#include <linux/videodev2.h>

#define HX_RAW_ABI_VERSION 2U
/* Bounded retained-frame capacity, matching the audited SPB FIFO. This is
 * allocation policy, not a change to HXST record layout or ioctl numbers.
 */
#define HX_RAW_QUEUE_DEPTH 2U
/* Master data excludes the three SPI transport bytes. Keep the old record
 * capacity for ABI compatibility; payload_size identifies bytes actually read.
 * A master-sized payload must have zero padding through HX_RAW_PAYLOAD_SIZE.
 */
#define HX_RAW_MASTER_SIZE 5060U
#define HX_RAW_PAYLOAD_SIZE 5132U
#define V4L2_META_FMT_HIMAX_SPI v4l2_fourcc('H', 'X', 'S', 'T')
#define HX_RAW_FRAME_SOS (1U << 0)
#define HX_RAW_FRAME_CHARGER (1U << 1)
/* Explicitly requested polling read, not a natural IRQ notification. */
#define HX_RAW_FRAME_REARM (1U << 2)
#define HX_RAW_FRAME_FLAGS (HX_RAW_FRAME_SOS | HX_RAW_FRAME_CHARGER | HX_RAW_FRAME_REARM)
#define HX_RAW_INVERT_X (1U << 0)
#define HX_RAW_INVERT_Y (1U << 1)
#define HX_RAW_SWAP_XY (1U << 2)
#define HX_RAW_COORDINATE_FLAGS (HX_RAW_INVERT_X | HX_RAW_INVERT_Y | HX_RAW_SWAP_XY)
#define V4L2_EVENT_HIMAX_STATE (V4L2_EVENT_PRIVATE_START + 0)

struct hx_raw_frame {
	__u32 size;
	__u32 version;
	__aligned_u64 epoch;
	__aligned_u64 sequence;
	__aligned_u64 timestamp_ns;
	__u32 flags;
	__u32 payload_size;
	__u32 reserved[2];
	__u8 payload[HX_RAW_PAYLOAD_SIZE];
	__u32 reserved_tail;
};

struct hx_raw_info {
	__u32 size;
	__u32 version;
	__u32 rows;
	__u32 cols;
	__u32 payload_size;
	__u32 coordinate_flags;
	char project_id[16];
};

enum hx_raw_action {
	HX_RAW_CLEAR_STATUS,
	HX_RAW_FORCE_FREQ,
	HX_RAW_FORCE_RATE,
	HX_RAW_ENTER_IDLE,
	HX_RAW_CALIBRATE,
	HX_RAW_RESET,
};

/* Narrow hardware commands only; no arbitrary AHB/firmware access. */
struct hx_raw_command {
	__u32 size;
	__u32 version;
	__aligned_u64 epoch;
	__aligned_u64 sequence;
	__u32 action;
	__u32 parameter;
	__u32 reserved[2];
};

/* Additive ABI v2 ioctl. A no-reset master read is allowed only when this
 * owner's producer is quiet and still exactly at the observed sequence.
 * sequence=0 is valid before the first frame of a running epoch.
 * Success means one complete SPI transfer was published, not solver validity.
 */
struct hx_raw_rearm {
	__u32 size;
	__u32 version;
	__aligned_u64 epoch;
	__aligned_u64 sequence;
	__u32 reserved[2];
};

/* Lifecycle events release userspace contacts immediately even if no frames
 * arrive while the panel is off. Event u.data begins with this structure.
 */
struct hx_raw_state {
	__u32 version;
	__u32 running;
	__aligned_u64 epoch;
};

#define VIDIOC_HIMAX_INFO _IOR('V', BASE_VIDIOC_PRIVATE, struct hx_raw_info)
#define VIDIOC_HIMAX_COMMAND _IOW('V', BASE_VIDIOC_PRIVATE + 1, struct hx_raw_command)
#define VIDIOC_HIMAX_REARM _IOW('V', BASE_VIDIOC_PRIVATE + 2, struct hx_raw_rearm)
/* Use a separate additive ioctl to negotiate origin-tagged frames. Legacy
 * REARM callers still receive the old flags; old modules return ENOTTY for
 * this request so new hosts can retain the conservative timeout fallback.
 */
#define VIDIOC_HIMAX_REARM_TAGGED _IOW('V', BASE_VIDIOC_PRIVATE + 3, struct hx_raw_rearm)

#endif
