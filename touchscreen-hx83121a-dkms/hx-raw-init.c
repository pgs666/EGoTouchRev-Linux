// SPDX-License-Identifier: GPL-2.0
#include "hx-raw-init.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#define HX_RAW_OUT		0x100072ecU
#define HX_SORTING_MODE		0x10007f04U
#define HX_NFRAME		0x10007294U
#define HX_RELOAD_DONE		0x100072c0U
#define HX_FW_STATUS		0x900000a8U
#define HX_RAW_PASSWORD		0x10000000U
#define HX_FLASH_RELOAD		0x10007f00U

static int write_u32(const struct hx_raw_init_bus *bus, u32 address, u32 value)
{
	u8 bytes[4] = { value, value >> 8, value >> 16, value >> 24 };

	return bus->write(bus->context, address, bytes, sizeof(bytes));
}

static int read_u32(const struct hx_raw_init_bus *bus, u32 address, u32 *value)
{
	u8 bytes[4];
	int ret;

	ret = bus->read(bus->context, address, bytes, sizeof(bytes));
	if (!ret)
		*value = (u32)bytes[0] | (u32)bytes[1] << 8 |
			 (u32)bytes[2] << 16 | (u32)bytes[3] << 24;
	return ret;
}

static int write_verify_low16(const struct hx_raw_init_bus *bus,
			      u32 address, u32 value)
{
	u32 readback;
	int ret;

	ret = write_u32(bus, address, value);
	if (ret)
		return ret;
	ret = read_u32(bus, address, &readback);
	if (ret)
		return ret;
	return (readback & 0xffff) == (value & 0xffff) ? 0 : -EINVAL;
}

static int retry_verify(const struct hx_raw_init_bus *bus,
			 u32 address, u32 value, unsigned int attempts)
{
	unsigned int i;

	/* Upstream inspection unlock/sorting retry ANY failed transaction;
	 * exhausted retries become VerificationFailed, even for bus errors.
	 * The outer lifecycle then starts a complete hardware-reset attempt.
	 */
	for (i = 0; i < attempts; i++)
		if (!write_verify_low16(bus, address, value))
			return 0;
	return -EINVAL;
}

int hx_raw_prepare_scan(const struct hx_raw_init_bus *bus,
			enum hx_raw_init_step *step)
{
	int ret;

	if (!bus || !step || !bus->write || !bus->read)
		return -EINVAL;
	*step = HX_RAW_INIT_FLASH_RELOAD;
	ret = write_u32(bus, HX_FLASH_RELOAD, 0);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_INSPECTION_UNLOCK;
	ret = retry_verify(bus, HX_RAW_PASSWORD, 0, 20);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_INSPECTION_SORTING;
	ret = retry_verify(bus, HX_SORTING_MODE, 0, 10);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_NFRAME_NORMAL;
	ret = write_verify_low16(bus, HX_NFRAME, 1);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_NFRAME_MODE;
	ret = write_verify_low16(bus, HX_NFRAME, 0x7f0c0001);
	if (!ret)
		*step = HX_RAW_INIT_COMPLETE;
	return ret;
}

static int clear_sorting(const struct hx_raw_init_bus *bus)
{
	int i, ret;
	u32 value;

	/* Keep the existing Linux bounded readback check. */
	for (i = 0; i < 3; i++) {
		ret = write_u32(bus, HX_SORTING_MODE, 0);
		if (ret)
			return ret;
		bus->sleep_ms(bus->context, 1);
		ret = read_u32(bus, HX_SORTING_MODE, &value);
		if (ret || !value)
			return ret;
	}
	return -EINVAL;
}

static int wait_u32(const struct hx_raw_init_bus *bus, u32 address,
		    u32 mask, u32 expected)
{
	int i, ret;
	u32 value;

	for (i = 0; i < 30; i++) {
		ret = read_u32(bus, address, &value);
		if (ret)
			return ret;
		if ((value & mask) == expected)
			return 0;
		bus->sleep_ms(bus->context, 10);
	}
	return -ETIMEDOUT;
}

int hx_raw_power_on(const struct hx_raw_init_bus *bus,
		    enum hx_raw_init_step *step)
{
	int ret;

	if (!bus || !step || !bus->write || !bus->read ||
	    !bus->sense_on || !bus->sleep_ms)
		return -EINVAL;

	/* Pinned upstream HimaxChipLifecycle::himax_mcu_power_on_init clears
	 * RawOut BEFORE sorting/N-frame/reload and sense_on(false). Leaving the
	 * old selection until after restart is not the same startup protocol.
	 */
	*step = HX_RAW_INIT_RAW_CLEAR;
	ret = write_u32(bus, HX_RAW_OUT, 0);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_SORTING_CLEAR;
	ret = clear_sorting(bus);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_NFRAME;
	ret = write_u32(bus, HX_NFRAME, 1);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_RELOAD_CLEAR;
	ret = write_u32(bus, HX_RELOAD_DONE, 0);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_SENSE_ON;
	ret = bus->sense_on(bus->context);
	if (ret)
		return ret;

	/* Retain Linux's additional central-state gate, and require the entire
	 * reload word, not just its low two bytes. These are readiness checks,
	 * not evidence that subsequent AFE calibration has completed.
	 */
	*step = HX_RAW_INIT_RUNNING;
	ret = wait_u32(bus, HX_FW_STATUS, 0xff, 5);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_RELOAD_DONE;
	ret = wait_u32(bus, HX_RELOAD_DONE, 0xffffffff, 0x72c0);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_RAW_SELECT;
	ret = write_u32(bus, HX_RAW_OUT, 0xf6);
	if (ret)
		return ret;
	*step = HX_RAW_INIT_PASSWORD;
	ret = write_u32(bus, HX_RAW_PASSWORD, 0x5aa5);
	if (!ret)
		*step = HX_RAW_INIT_COMPLETE;
	return ret;
}

const char *hx_raw_init_step_name(enum hx_raw_init_step step)
{
	static const char * const names[] = {
		"raw_clear", "sorting_clear", "nframe", "reload_clear",
		"sense_on", "running", "reload_done", "raw_select",
		"raw_password", "flash_reload", "inspection_unlock",
		"inspection_sorting", "nframe_normal", "nframe_mode", "complete",
	};

	if ((unsigned int)step > HX_RAW_INIT_COMPLETE)
		return "invalid";
	return names[step];
}
