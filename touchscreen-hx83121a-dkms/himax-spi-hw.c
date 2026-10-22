// SPDX-License-Identifier: GPL-2.0
/* HX83121A SPI transport, register access and firmware control. */

#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

#include "himax-spi.h"
#include "hx-afe.h"
#include "hx-otp.h"
#include "hx-raw-init.h"
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
#include "hx-ahb.h"
#else
#include "hx-panel-profile.h"
#endif

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
static bool himax_afe_mailbox_debug;
module_param_named(afe_mailbox_debug, himax_afe_mailbox_debug, bool, 0600);
MODULE_PARM_DESC(afe_mailbox_debug,
		"Log existing AFE mailbox readback (no extra hardware reads)");
#else
#define himax_afe_mailbox_debug false
#endif

#define HIMAX_BUS_RETRY					3
/* SPI bus read header length */
#define HIMAX_BUS_R_HLEN				3U
/* SPI bus write header length */
#define HIMAX_BUS_W_HLEN				2U
/* TP SRAM address size and data size */
#define HIMAX_REG_SZ					4U
#define HIMAX_HX83121A_SAFE_MODE_PASSWORD		0x9527

/* Clear 4 bytes data */
#define HIMAX_DATA_CLEAR				0x00000000
/* AHB register addresses */
#define HIMAX_AHB_ADDR_BYTE_0				0x00
#define HIMAX_AHB_ADDR_RDATA_BYTE_0			0x08
#define HIMAX_AHB_ADDR_ACCESS_DIRECTION			0x0c
#define HIMAX_AHB_ADDR_INCR4				0x0d
#define HIMAX_AHB_ADDR_CONTI				0x13
#define HIMAX_AHB_ADDR_EVENT_STACK			0x30
#define HIMAX_AHB_ADDR_PSW_LB				0x31

/* AHB register values/commands */
#define HIMAX_AHB_CMD_ACCESS_DIRECTION_READ		0x00
#define HIMAX_AHB_CMD_CONTI				0x31
#define HIMAX_AHB_CMD_INCR4				0x10
#define HIMAX_AHB_CMD_INCR4_ADD_4_BYTE			0x01
#define HIMAX_AHB_CMD_LEAVE_SAFE_MODE			0x0000

/* DSRAM flag addresses */
#define HIMAX_DSRAM_ADDR_FLASH_RELOAD			0x10007f00
#define HIMAX_DSRAM_ADDR_SORTING_MODE_EN		0x10007f04
#define HIMAX_DSRAM_ADDR_SET_NFRAME			0x10007294
#define HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD		0x10000000
#define HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND		0x10007fd0
#define HIMAX_DSRAM_ADDR_USB_DETECT			0x10007f38

/* dsram flag data */
#define HIMAX_DSRAM_DATA_AP_RESUMED			0x00000000
#define HIMAX_DSRAM_DATA_USB_CONNECTED			0xa55aa55a
/* hx83121a-specific register/dsram flags/data */
#define HIMAX_HX83121A_DSRAM_ADDR_RAW_OUT_SEL		0x100072ec

/* Firmware AFE commands used by the Windows runtime. */
#define HIMAX_HX83121A_AFE_CMD_START_CALIBRATION	0x01
#define HIMAX_HX83121A_AFE_CMD_FORCE_SCAN_RATE		0x0e
#define HIMAX_HX83121A_AFE_SCAN_RATE_120HZ		0x00

/* hardware register addresses */
#define HIMAX_REG_ADDR_RELOAD_STATUS			0x80050000
#define HIMAX_REG_ADDR_RELOAD_CRC32_RESULT		0x80050018
#define HIMAX_REG_ADDR_RELOAD_ADDR_FROM			0x80050020
#define HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT		0x80050028
#define HIMAX_REG_ADDR_CTRL_FW				0x9000005c
#define HIMAX_REG_ADDR_FW_STATUS			0x900000a8
#define HIMAX_REG_ADDR_ICID				0x900000d0

/* hardware reg data/flags */
#define HIMAX_REG_DATA_FW_STATE_RUNNING			0x05
#define HIMAX_REG_DATA_FW_STATE_SAFE_MODE		0x0c
#define HIMAX_REG_DATA_FW_RE_INIT			0x00
#define HIMAX_REG_DATA_FW_GO_SAFEMODE			0xa5
#define HIMAX_REG_DATA_FW_IN_SAFEMODE			0x87
#define HIMAX_REG_DATA_RELOAD_DONE			0x01
#define HIMAX_REG_DATA_RELOAD_PASSWORD			0x99

/* SPI CS setup time */
/* HIMAX SPI function select, 1st byte of any SPI command sequence */
#define HIMAX_SPI_FUNCTION_READ				0xf3
#define HIMAX_SPI_FUNCTION_WRITE			0xf2

union himax_dword_data {
	u32 dword;
	u16 word[2];
	u8 byte[4];
};

static int himax_mcu_write_verify_u32(struct himax_ts_data *ts, u32 addr,
					      u32 value, int attempts);
static int himax_sense_off(struct himax_ts_data *ts, bool check_en);
static int himax_sense_on(struct himax_ts_data *ts, bool sw_reset);

int himax_spi_read(struct himax_ts_data *ts, u8 cmd, u8 *buf, u32 len)
{
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len = HIMAX_BUS_R_HLEN + len,
		.tx_buf = ts->xfer_buf,
		.rx_buf = ts->xfer_buf,
		.cs_change = 0,
	};
	int retry_cnt;
	int ret;

	if (len > ts->xfer_buf_sz - HIMAX_BUS_R_HLEN)
		return -EMSGSIZE;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	for (retry_cnt = 0; retry_cnt < HIMAX_BUS_RETRY; retry_cnt++) {
		/* RX aliases TX; a failed transfer may have overwritten the header. */
		memset(ts->xfer_buf, 0, HIMAX_BUS_R_HLEN + len);
		ts->xfer_buf[0] = HIMAX_SPI_FUNCTION_READ;
		ts->xfer_buf[1] = cmd;
		ret = spi_sync(ts->spi, &msg);
		if (ret < 0)
			dev_err_ratelimited(ts->dev,
					    "SPI read failed: %d (attempt %d/%d)\n",
					    ret, retry_cnt + 1, HIMAX_BUS_RETRY);
		else
			break;
	}

	if (ret < 0)
		return ret;

	/* Destination may alias xfer_buf when caller reuses driver buffers. */
	memmove(buf, ts->xfer_buf + HIMAX_BUS_R_HLEN, len);
	return 0;
}

/*
 * 1st byte is the spi function select and 2nd byte is the command belong to the
 * spi function. Else is the data to write.
 */
static int himax_spi_write(struct himax_ts_data *ts, u8 *tx_buf, u32 len)
{
	int ret;
	struct spi_message msg;
	struct spi_transfer xfer = {
		.tx_buf = tx_buf,
		.len = len,
		.cs_change = 0,
	};

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(ts->spi, &msg);
	if (ret < 0) {
		dev_err(ts->dev, "SPI write failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int himax_write(struct himax_ts_data *ts, u8 cmd, const u8 *addr,
		       const u8 *data, u32 len)
{
	u8 *ptr = ts->xfer_buf;

	if (len > ts->xfer_buf_sz - HIMAX_BUS_W_HLEN || (addr && len < 4))
		return -EMSGSIZE;
	memset(ts->xfer_buf, 0, len + HIMAX_BUS_W_HLEN);
	ts->xfer_buf[0] = HIMAX_SPI_FUNCTION_WRITE;
	ts->xfer_buf[1] = cmd;
	ptr += HIMAX_BUS_W_HLEN;
	len += HIMAX_BUS_W_HLEN;

	if (addr) {
		memcpy(ptr, addr, 4);
		ptr += 4;
	}

	if (data)
		memcpy(ptr, data, len - (ptr - ts->xfer_buf));

	return himax_spi_write(ts, ts->xfer_buf, len);
}

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
static int himax_ahb_write(void *context, u8 command, const u8 *address,
			  const u8 *data, u32 length)
{
	struct himax_ts_data *ts = context;
	u32 address_length = address ? HIMAX_REG_SZ : 0;

	/* Check before addition/conversion, including a maximal u32 length. */
	if (ts->xfer_buf_sz < HIMAX_BUS_W_HLEN + address_length ||
	    length > ts->xfer_buf_sz - HIMAX_BUS_W_HLEN - address_length)
		return -EMSGSIZE;
	return himax_write(ts, command, address, data, length + address_length);
}

static int himax_ahb_read(void *context, u8 command, u8 *data, u32 length)
{
	return himax_spi_read(context, command, data, length);
}

static void himax_ahb_sleep(void *context, unsigned int milliseconds)
{
	usleep_range(milliseconds * 1000, milliseconds * 1000 + 100);
}

static struct hx_ahb_bus himax_ahb_bus(struct himax_ts_data *ts)
{
	return (struct hx_ahb_bus) {
		.context = ts,
		.write = himax_ahb_write,
		.read = himax_ahb_read,
		.sleep_ms = himax_ahb_sleep,
	};
}

static int himax_mcu_register_read(struct himax_ts_data *ts, u32 addr, u8 *buf, u32 len)
{
	const struct hx_ahb_bus bus = himax_ahb_bus(ts);

	return hx_ahb_register_read(&bus, addr, buf, len);
}

static int himax_mcu_register_write(struct himax_ts_data *ts, u32 addr,
				   const u8 *buf, u32 len)
{
	const struct hx_ahb_bus bus = himax_ahb_bus(ts);

	return hx_ahb_register_write(&bus, addr, buf, len);
}
#else
/**
 * himax_mcu_set_burst_mode() - Set burst mode for MCU
 * @ts: Himax touch screen data
 * @auto_add_4_byte: Enable auto add 4 byte mode
 *
 * Set burst mode for MCU, which is used for read/write data from/to MCU.
 * HIMAX_AHB_ADDR_CONTI config the IC to take data continuously,
 * HIMAX_AHB_ADDR_INCR4 config the IC to auto increment the address by 4 byte when
 * each 4 bytes read/write.
 *
 * Return: 0 on success, negative error code on failure
 */
static int himax_mcu_set_burst_mode(struct himax_ts_data *ts, bool auto_add_4_byte)
{
	int ret;
	u8 tmp;

	tmp = HIMAX_AHB_CMD_CONTI;
	ret = himax_write(ts, HIMAX_AHB_ADDR_CONTI, NULL, &tmp, 1);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_conti failed\n", __func__);
		return ret;
	}

	tmp = HIMAX_AHB_CMD_INCR4;
	if (auto_add_4_byte)
		tmp |= HIMAX_AHB_CMD_INCR4_ADD_4_BYTE;

	ret = himax_write(ts, HIMAX_AHB_ADDR_INCR4, NULL, &tmp, 1);
	if (ret < 0)
		dev_err(ts->dev, "%s: write ahb_addr_incr4 failed\n", __func__);

	return ret;
}

/*
 * Read data from himax internal registers and SRAM
 *
 * We write the (SRAM)address to AHB register to tell where to read. Then set
 * the access direction to read, and read the data from AHB register.
 */
static int himax_mcu_register_read(struct himax_ts_data *ts, u32 addr, u8 *buf, u32 len)
{
	int ret;
	u8 direction_switch = HIMAX_AHB_CMD_ACCESS_DIRECTION_READ;
	union himax_dword_data target_addr;

	ret = himax_mcu_set_burst_mode(ts, len > HIMAX_REG_SZ);
	if (ret)
		return ret;

	target_addr.dword = cpu_to_le32(addr);
	ret = himax_write(ts, HIMAX_AHB_ADDR_BYTE_0, target_addr.byte, NULL, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);
		return ret;
	}

	ret = himax_write(ts, HIMAX_AHB_ADDR_ACCESS_DIRECTION, NULL,
				  &direction_switch, 1);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write ahb_addr_access_direction failed\n", __func__);
		return ret;
	}

	ret = himax_spi_read(ts, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf, len);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read ahb_addr_rdata_byte_0 failed\n", __func__);
		return ret;
	}

	return himax_mcu_set_burst_mode(ts, !(len > HIMAX_REG_SZ));
}

/* Write the internal (SRAM)address and data to AHB register */
static int himax_mcu_register_write(struct himax_ts_data *ts, u32 addr, const u8 *buf, u32 len)
{
	int ret, restore;

	union himax_dword_data target_addr;

	ret = himax_mcu_set_burst_mode(ts, len > HIMAX_REG_SZ);
	if (ret)
		return ret;

	target_addr.dword = cpu_to_le32(addr);
	ret = himax_write(ts, HIMAX_AHB_ADDR_BYTE_0,
				target_addr.byte, buf, len + HIMAX_REG_SZ);
	if (ret < 0)
		dev_err(ts->dev, "%s: write ahb_addr_byte_0 failed\n", __func__);

	restore = himax_mcu_set_burst_mode(ts, !(len > HIMAX_REG_SZ));
	return ret ? ret : restore;
}
#endif

static int himax_otp_bus_read(void *context, u8 command, u8 *data, size_t length)
{
	return himax_spi_read(context, command, data, length);
}

static int himax_otp_bus_write(void *context, u8 command, const u8 *data,
			       size_t length)
{
	return himax_write(context, command, NULL, data, length);
}

static void himax_otp_sleep(void *context, unsigned int milliseconds)
{
	if (milliseconds >= 20)
		msleep(milliseconds);
	else
		usleep_range(milliseconds * 1000, milliseconds * 1000 + 100);
}

int himax_get_project_id_otp(struct himax_ts_data *ts, char *project_id,
				     size_t project_id_len)
{
	const struct hx_otp_bus bus = {
		.context = ts,
		.read = himax_otp_bus_read,
		.write = himax_otp_bus_write,
		.sleep_ms = himax_otp_sleep,
	};
	struct hx_otp_identity identity;
	int ret;

	if (!project_id || project_id_len < 11)
		return -EINVAL;
	project_id[0] = '\0';
	ret = hx_otp_read_project(&bus, &identity);
	if (ret) {
		dev_warn(ts->dev, "panel OTP failed: %d slot=0x%04x id=%*phN\n",
			 ret, identity.slot, 10, identity.project_id);
		return ret;
	}
	strscpy(project_id, identity.project_id, project_id_len);
	dev_info(ts->dev,
		 "panel OTP: %s slot=0x%04x revision=0x%02x E2=0x%02x valid=0x%x\n",
		 project_id, identity.slot, identity.revision, identity.e2,
		 identity.extra_valid);
	return 0;
}

/* Xiaomi replays firmware-owned state with write/read retry loops.  Keep the
 * same rule, but require an exact readback (their AP-notify loop accidentally
 * stops on a successful read even when the value differs).
 */
static int himax_mcu_write_verify_u32(struct himax_ts_data *ts, u32 addr,
				      u32 value, int attempts)
{
	union himax_dword_data expected = { .dword = cpu_to_le32(value) };
	union himax_dword_data actual;
	int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < attempts; attempt++) {
		ret = himax_mcu_register_write(ts, addr, expected.byte,
					       sizeof(expected.byte));
		if (ret)
			continue;
		usleep_range(1000, 1100);
		ret = himax_mcu_register_read(ts, addr, actual.byte,
					      sizeof(actual.byte));
		if (!ret && actual.dword == expected.dword)
			return 0;
		if (!ret)
			ret = -EIO;
	}

	dev_err(ts->dev, "register 0x%08x failed readback for 0x%08x\n",
		addr, value);
	return ret;
}

static int himax_reg_write_cb(void *context, u32 address, const u8 *data,
			      size_t length)
{
	return himax_mcu_register_write(context, address, data, length);
}

static int himax_afe_reg_read(void *context, u32 address, u8 *data, size_t length)
{
	struct himax_ts_data *ts = context;
	int ret;

	ret = himax_mcu_register_read(ts, address, data, length);
	/* Opt-in diagnostic build: reuse the protocol's existing readback.
	 * Do not add register polling or interpret this as firmware completion.
	 */
	if (!ret && himax_afe_mailbox_debug)
		dev_info(ts->dev, "AFE mailbox addr=0x%08x readback=%*ph\n",
			address, (int)length, data);
	return ret;
}

static int himax_afe_send_command(struct himax_ts_data *ts, u8 *slot,
				  u8 command, u8 value)
{
	const struct hx_afe_bus bus = {
		.context = ts,
		.write = himax_reg_write_cb,
		.read = himax_afe_reg_read,
	};
	u8 sent_slot = *slot;
	int ret;

	ret = hx_afe_send(&bus, slot, command, value);
	if (himax_afe_mailbox_debug)
		dev_info(ts->dev, "AFE command=0x%02x value=0x%02x slot=%u next=%u transport=%d\n",
			command, value, sent_slot, *slot, ret);
	return ret;
}

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
int himax_set_charger(struct himax_ts_data *ts, bool connected)
{
	int ret;

	ret = himax_mcu_write_verify_u32(ts, HIMAX_DSRAM_ADDR_USB_DETECT,
		connected ? HIMAX_DSRAM_DATA_USB_CONNECTED : 0, 3);
	hx_raw_charger_write_result(&ts->charger, connected, ret);
	return ret;
}

int himax_afe_command(struct himax_ts_data *ts, u32 action, u32 parameter)
{
	static const u8 commands[] = { 0x06, 0x0c, 0x0e, 0x0a, 0x01 };

	if (action >= ARRAY_SIZE(commands))
		return -EINVAL;
	if (action == HX_RAW_CLEAR_STATUS) {
		if (parameter != 4 && parameter != 8)
			return -EINVAL;
	} else if (action == HX_RAW_FORCE_FREQ) {
		if (parameter > 1)
			return -EINVAL;
	} else if (parameter) {
		return -EINVAL;
	}
	return himax_afe_send_command(ts, &ts->afe_command_slot,
				      commands[action], parameter);
}
#else
int himax_restore_afe_runtime(struct himax_ts_data *ts)
{
	int ret;

	if (!himax_restore_afe_enabled())
		return 0;

	/* StartCalibration is intentionally absent here.  The command transport
	 * has no calibration-complete acknowledgement in the recovered screen-on
	 * path; issuing it and immediately consuming raw frames races the AFE.
	 */
	ret = himax_afe_send_command(ts, &ts->afe_command_slot,
				     HIMAX_HX83121A_AFE_CMD_FORCE_SCAN_RATE,
				     HIMAX_HX83121A_AFE_SCAN_RATE_120HZ);
	if (ret)
		dev_warn(ts->dev, "failed to force 120 Hz scan rate: %d\n", ret);
	else
		dev_info(ts->dev, "120 Hz scan-rate command transport completed\n");

	return ret;
}

int himax_process_blrecal_request(struct himax_ts_data *ts)
{
	int ret;

	if (!ts->algo->blrecal_requested)
		return 0;
	/* BLRecal_Process raises a request only after its complete 5+10 frame
	 * vote.  Consume it at the hardware owner, never in the IRQ algorithm,
	 * and acknowledge only after both command writes and the slot readback
	 * complete.  The protocol exposes no calibration-done bit, so this is
	 * explicitly a transport acknowledgement rather than a false claim that
	 * AFE convergence has finished.
	 */
	ret = himax_afe_send_command(ts, &ts->afe_command_slot,
				     HIMAX_HX83121A_AFE_CMD_START_CALIBRATION, 0);
	if (ret) {
		dev_warn_ratelimited(ts->dev,
			"BLRecal calibration command transport failed: %d\n", ret);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		ts->afe_calibration_failures++;
#endif
		return ret;
	}
	hx_blrecal_ack(ts->algo, true);
	ts->algo->baseline_update_suppressed_once = true;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ts->afe_calibration_count++;
#endif
	return 0;
}
#endif /* CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2 */

/*
 * Wakeup IC bus interface. The IC may enter sleep mode and need to wakeup
 * before any operation.
 */
static int himax_mcu_interface_on(struct himax_ts_data *ts)
{
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
	const struct hx_ahb_bus bus = himax_ahb_bus(ts);

	return hx_ahb_interface_on(&bus);
#else
	int ret;
	u8 buf[HIMAX_REG_SZ];
	u8 buf2[HIMAX_REG_SZ];
	u32 retry_cnt;
	const u32 burst_retry_limit = 10;

	/* Read a dummy register to wake up BUS. */
	ret = himax_spi_read(ts, HIMAX_AHB_ADDR_RDATA_BYTE_0, buf, 4);
	if (ret < 0)
		goto err;

	for (retry_cnt = 0; retry_cnt < burst_retry_limit; retry_cnt++) {
		ret = himax_mcu_set_burst_mode(ts, false);
		if (ret < 0)
			goto err;

		/* Check cmd */
		ret = himax_spi_read(ts, HIMAX_AHB_ADDR_CONTI, buf, 1);
		if (ret < 0)
			goto err;

		ret = himax_spi_read(ts, HIMAX_AHB_ADDR_INCR4, buf2, 1);
		if (ret < 0)
			goto err;

		if (buf[0] == HIMAX_AHB_CMD_CONTI && buf2[0] == HIMAX_AHB_CMD_INCR4)
			return 0;

		usleep_range(1000, 1100);
	}

err:
	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
#endif
}

static void himax_pin_reset(struct himax_ts_data *ts)
{
	gpiod_set_value_cansleep(ts->gpiod_rst, 1);
	usleep_range(20000, 20100);
	gpiod_set_value_cansleep(ts->gpiod_rst, 0);
	usleep_range(50000, 50100);
}

void himax_int_enable(struct himax_ts_data *ts, bool enable)
{
	unsigned long flags;

	spin_lock_irqsave(&ts->irq_lock, flags);
	if (enable && ts->irq_enabled == false)
		enable_irq(ts->spi->irq);
	else if (!enable && ts->irq_enabled == true)
		disable_irq_nosync(ts->spi->irq);
	ts->irq_enabled = enable;
	spin_unlock_irqrestore(&ts->irq_lock, flags);
}

static void himax_quiesce_irq(struct himax_ts_data *ts)
{
	himax_int_enable(ts, false);
	synchronize_irq(ts->spi->irq);
}

void himax_lock(struct himax_ts_data *ts)
{
	himax_quiesce_irq(ts);
	mutex_lock(&ts->op_lock);
	/*
	 * Another lifecycle operation may have enabled IRQ while we waited for
	 * op_lock. Disable it again; any newly queued IRQ thread also needs this
	 * mutex, so it cannot enter a hardware transaction until we unlock.
	 * Do not synchronize_irq() here: that thread could be waiting on us.
	 */
	himax_int_enable(ts, false);
}

void himax_unlock(struct himax_ts_data *ts)
{
	mutex_unlock(&ts->op_lock);
}

static void himax_mcu_ic_reset(struct himax_ts_data *ts, bool int_off)
{
	if (int_off)
		himax_int_enable(ts, false);

	himax_pin_reset(ts);

	if (int_off)
		himax_int_enable(ts, true);
}

/**
 * sense_off: stop MCU
 * 1. request FW to stop
 * 2. enter safe mode (and reset TCON for some ICs).
 *
 * @check_en: confirm if the FW is stopped
 */
static int himax_sense_off(struct himax_ts_data *ts, bool check_en)
{
	int ret;
	u32 retry_cnt;
	const u32 stop_fw_retry_limit = 35;
	const u32 enter_safe_mode_retry_limit = 5;
	const union himax_dword_data safe_mode = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_FW_GO_SAFEMODE)
	};
	union himax_dword_data data;

	dev_info(ts->dev, "%s: check %s\n", __func__, check_en ? "True" : "False");
	if (!check_en)
		goto without_check;

	for (retry_cnt = 0; retry_cnt < stop_fw_retry_limit; retry_cnt++) {
		if (retry_cnt == 0 ||
		    (data.byte[0] != HIMAX_REG_DATA_FW_GO_SAFEMODE &&
		    data.byte[0] != HIMAX_REG_DATA_FW_RE_INIT &&
		    data.byte[0] != HIMAX_REG_DATA_FW_IN_SAFEMODE)) {
			ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_CTRL_FW,
						       safe_mode.byte, 4);
			if (ret < 0) {
				dev_err(ts->dev, "%s: stop FW failed\n", __func__);
				return ret;
			}
		}
		usleep_range(10000, 11000);

		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read central state failed\n", __func__);
			return ret;
		}
		if (data.byte[0] != HIMAX_REG_DATA_FW_STATE_RUNNING) {
			dev_info(ts->dev, "%s: Do not need wait FW, Status = 0x%02X!\n", __func__,
			  data.byte[0]);
			break;
		}

		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_CTRL_FW, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read ctrl FW failed\n", __func__);
			return ret;
		}
		if (data.byte[0] == HIMAX_REG_DATA_FW_IN_SAFEMODE)
			break;
	}

	if (data.byte[0] != HIMAX_REG_DATA_FW_IN_SAFEMODE)
		dev_warn(ts->dev, "%s: Failed to stop FW!\n", __func__);

without_check:
	for (retry_cnt = 0; retry_cnt < enter_safe_mode_retry_limit; retry_cnt++) {
		/* set Enter safe mode : 0x31 ==> 0x9527 */
		data.word[0] = cpu_to_le16(HIMAX_HX83121A_SAFE_MODE_PASSWORD);
		ret = himax_write(ts, HIMAX_AHB_ADDR_PSW_LB, NULL, data.byte, 2);
		if (ret < 0) {
			dev_err(ts->dev, "%s: enter safe mode failed\n", __func__);
			return ret;
		}

		/* Check enter_save_mode */
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_FW_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read central state failed\n", __func__);
			return ret;
		}

		if (data.byte[0] == HIMAX_REG_DATA_FW_STATE_SAFE_MODE) {
			dev_info(ts->dev, "%s: Safe mode entered\n", __func__);
			return 0;
		}

		usleep_range(10000, 10100);
		himax_pin_reset(ts);
	}
	dev_err(ts->dev, "%s: failed!\n", __func__);

	return -EIO;
}

/**
 * @sw_reset: true for software reset, false for hardware reset
 *     true: write IC to leave safe mode
 *     false: pin reset
 *
 * make MCU restart running the FW
 */
static int himax_sense_on(struct himax_ts_data *ts, bool sw_reset)
{
	int ret;
	const union himax_dword_data re_init = {
		.dword = cpu_to_le32(HIMAX_REG_DATA_FW_RE_INIT)
	};
	union himax_dword_data data;

	ret = himax_mcu_interface_on(ts);
	if (ret < 0)
		return ret;

	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_CTRL_FW, re_init.byte, 4);
	if (ret < 0)
		return ret;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
	/* Active upstream hx_sense_on waits 11 ms before the reset pair. */
	usleep_range(11000, 12000);
#else
	usleep_range(10000, 11000);
#endif
	if (!sw_reset) {
		himax_mcu_ic_reset(ts, false);
	} else {
		data.word[0] = cpu_to_le16(HIMAX_AHB_CMD_LEAVE_SAFE_MODE);
		ret = himax_write(ts, HIMAX_AHB_ADDR_PSW_LB, NULL, data.byte, 2);
		if (ret < 0)
			return ret;
	}
	return 0;
}

int hx83121a_chip_detect(struct himax_ts_data *ts)
{
	int ret;
	u32 retry_cnt;
	const u32 read_icid_retry_limit = 5;
	union himax_dword_data data;

	himax_pin_reset(ts);

	ret = himax_mcu_interface_on(ts);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read ahb_addr_conti failed\n", __func__);
		return ret;
	}

	ret = himax_sense_off(ts, false);
	if (ret)
		return ret;

	for (retry_cnt = 0; retry_cnt < read_icid_retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_ICID, data.byte, 4);
		if (ret) {
			dev_err(ts->dev, "%s: Read IC ID Fail\n", __func__);
			return ret;
		}

		data.dword = le32_to_cpu(data.dword) >> 8;
		dev_info(ts->dev, "Detected IC HX%06X\n", data.dword);

		if (data.dword == 0x83121a)
			return 0;
	}
	return -ENODEV;
}

int himax_verify_running_ic(struct himax_ts_data *ts)
{
	union himax_dword_data data;
	int ret;

	ret = himax_mcu_interface_on(ts);
	if (ret)
		return ret;
	ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_ICID, data.byte,
				      sizeof(data.byte));
	if (ret)
		return ret;
	if ((le32_to_cpu(data.dword) >> 8) != 0x83121a)
		return -ENODEV;

	return 0;
}

int himax_restore_raw_runtime(struct himax_ts_data *ts)
{
	union himax_dword_data password = {
		.dword = cpu_to_le32(0x00005aa5),
	};
	int ret;
	int supplied;

	/* These are firmware-owned runtime values.  Xiaomi similarly replays its
	 * USB/gesture/touch-mode values after resume; our raw-matrix transport
	 * additionally needs RawOut and the SRAM producer handshake.
	 */
	ret = himax_mcu_write_verify_u32(ts,
		HIMAX_DSRAM_ADDR_AP_NOTIFY_FW_SUSPEND,
		HIMAX_DSRAM_DATA_AP_RESUMED, 3);
	if (ret)
		return ret;
	ret = himax_mcu_write_verify_u32(ts,
		HIMAX_DSRAM_ADDR_SORTING_MODE_EN, HIMAX_DATA_CLEAR, 3);
	if (ret)
		return ret;
	ret = himax_mcu_write_verify_u32(ts, HIMAX_DSRAM_ADDR_SET_NFRAME,
					 1, 3);
	if (ret)
		return ret;
	ret = himax_mcu_write_verify_u32(ts,
		HIMAX_HX83121A_DSRAM_ADDR_RAW_OUT_SEL, 0xf6, 3);
	if (ret)
		return ret;

	/* dagu replays charger state after every resume to select the firmware's
	 * anti-USB-interference profile.  It is supplementary to our host-side
	 * common-mode filter, not a replacement for it.
	 */
	supplied = power_supply_is_system_supplied();
	if (supplied >= 0) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
		ts->charger.connected = supplied > 0;
		ret = himax_set_charger(ts, ts->charger.connected);
#else
		struct hx_baseline_platform_state state = ts->algo->platform;

		state.charger_connected = supplied > 0;
		hx_safe_baseline_set_platform_state(ts->algo, &state);
		ret = himax_mcu_write_verify_u32(ts,
			HIMAX_DSRAM_ADDR_USB_DETECT,
			supplied ? HIMAX_DSRAM_DATA_USB_CONNECTED : 0, 3);
#endif
		if (ret)
			dev_warn(ts->dev,
				 "failed to replay charger interference mode: %d\n",
				 ret);
	}

	/* The password is a producer/consumer handshake, not persistent config;
	 * firmware may consume it immediately, so an equality readback would be
	 * incorrect.  Validate it through fresh master event frames below.
	 */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_RAWDATA_PASSWORD,
					password.byte, sizeof(password.byte));
}

/* Match the Windows Chip::Init() resume path: every hardware reinit must
 * explicitly allow the controller to rebuild its runtime state from flash.
 * Keeping 0x5aa5 ("has flash, but not reload") across a reset can leave a
 * partially retained firmware state after display sleep.
 */
int himax_enable_fw_reload(struct himax_ts_data *ts)
{
	union himax_dword_data data = { .dword = cpu_to_le32(0) };

	/* This flag is firmware-owned once sensing restarts, so mirror Windows and
	 * verify success through the subsequent 0x72c0 reload-done handshake.
	 */
	return himax_mcu_register_write(ts, HIMAX_DSRAM_ADDR_FLASH_RELOAD,
					data.byte, 4);
}

static int himax_raw_init_read(void *context, u32 address, u8 *data, size_t length)
{
	return himax_mcu_register_read(context, address, data, length);
}

static int himax_raw_init_sense_on(void *context)
{
	return himax_sense_on(context, false);
}

static void himax_raw_init_sleep(void *context, unsigned int milliseconds)
{
	usleep_range(milliseconds * 1000, milliseconds * 1100);
}

int himax_mcu_power_on_init(struct himax_ts_data *ts)
{
	const struct hx_raw_init_bus bus = {
		.context = ts,
		.write = himax_reg_write_cb,
		.read = himax_raw_init_read,
		.sense_on = himax_raw_init_sense_on,
		.sleep_ms = himax_raw_init_sleep,
	};
	enum hx_raw_init_step step = HX_RAW_INIT_RAW_CLEAR;
	int ret;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2
	ret = hx_raw_start_scan(&bus, &step);
#else
	ret = hx_raw_power_on(&bus, &step);
#endif
	if (ret)
		dev_err(ts->dev, "raw power-on %s failed: %d\n",
			hx_raw_init_step_name(step), ret);
	else
		dev_info(ts->dev, "raw power-on ready\n");
	return ret;
}

int himax_mcu_check_crc(struct himax_ts_data *ts, u32 start_addr,
			int reload_length, u32 *crc_result)
{
	int ret;
	int length = reload_length / HIMAX_REG_SZ;
	u32 retry_cnt;
	const u32 retry_limit = 100;
	union himax_dword_data data, addr;

	addr.dword = cpu_to_le32(start_addr);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_RELOAD_ADDR_FROM, addr.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write reload start address fail\n", __func__);
		return ret;
	}

	data.word[1] = cpu_to_le16(HIMAX_REG_DATA_RELOAD_PASSWORD);
	data.word[0] = cpu_to_le16(length);
	ret = himax_mcu_register_write(ts, HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: write reload length and password fail!\n", __func__);
		return ret;
	}

	ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_ADDR_CMD_BEAT, data.byte, 4);
	if (ret < 0) {
		dev_err(ts->dev, "%s: read reload length and password fail!\n", __func__);
		return ret;
	}

	if (le16_to_cpu(data.word[0]) != length) {
		dev_err(ts->dev, "%s: length verify failed!\n", __func__);
		return -EINVAL;
	}

	for (retry_cnt = 0; retry_cnt < retry_limit; retry_cnt++) {
		ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_STATUS, data.byte, 4);
		if (ret < 0) {
			dev_err(ts->dev, "%s: read reload status fail!\n", __func__);
			return ret;
		}

		data.dword = le32_to_cpu(data.dword);
		if ((data.byte[0] & HIMAX_REG_DATA_RELOAD_DONE) != HIMAX_REG_DATA_RELOAD_DONE) {
			ret = himax_mcu_register_read(ts, HIMAX_REG_ADDR_RELOAD_CRC32_RESULT,
						      data.byte, HIMAX_REG_SZ);
			if (ret < 0) {
				dev_err(ts->dev, "%s: read crc32 result fail!\n", __func__);
				return ret;
			}
			*crc_result = le32_to_cpu(data.dword);
			return 0;
		}

		dev_info(ts->dev, "%s: Waiting for HW ready!\n", __func__);
		usleep_range(1000, 1100);
	}

	dev_err(ts->dev, "%s: read FW status fail\n", __func__);
	return -EINVAL;
}
