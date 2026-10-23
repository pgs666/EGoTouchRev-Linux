// SPDX-License-Identifier: GPL-2.0
/*
 * HX83121A raw SPI transport. No baseline, finger detection or input reporting
 * lives here. A userspace solver consumes timestamped HXST metadata buffers.
 *
 * Lock order: video_lock -> op_lock -> buffer_lock. The IRQ never takes
 * video_lock, and no hardware transaction is performed under buffer_lock.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "himax-spi.h"

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
static unsigned int himax_afe_start_failures;
module_param_named(afe_start_failures, himax_afe_start_failures, uint, 0400);
MODULE_PARM_DESC(afe_start_failures,
		"Diagnostic only: fail the first 0..8 AFE attempts before SPI, per device");
#endif

struct himax_video_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head link;
};

static void himax_state_event(struct himax_ts_data *ts, bool running)
{
	struct v4l2_event event = { .type = V4L2_EVENT_HIMAX_STATE };
	struct hx_raw_state state = {
		.version = HX_RAW_ABI_VERSION,
		.running = running,
		.epoch = ts->epoch,
	};

	memcpy(event.u.data, &state, sizeof(state));
	v4l2_event_queue(ts->video, &event);
}

static int himax_read_raw(struct himax_ts_data *ts)
{
	int ret;

	/* The complete master is 5063 wire bytes, including the SPI prefix.
	 * Do not issue another event-stack command for the legacy ABI tail.
	 * himax_spi_read publishes only a successful, complete transaction.
	 */
	ret = himax_spi_read(ts, HIMAX_EVENT_STACK_ADDRESS,
			     ts->event_buf, HX_RAW_MASTER_SIZE);
	if (ret)
		return ret;
	memset(ts->event_buf + HX_RAW_MASTER_SIZE, 0,
	       HX_RAW_PAYLOAD_SIZE - HX_RAW_MASTER_SIZE);
	return 0;
}

#include "himax-v4l2-startup.h"

static void himax_publish_frame(struct himax_ts_data *ts, u64 now, bool rearm);

/* IRQs must be disabled and op_lock held. No algorithm qualification here:
 * the upstream solver owns validity, calibration state and baseline policy.
 */
static int himax_start_afe(struct himax_ts_data *ts)
{
	int ret;

	hx_raw_startup_cancel(&ts->startup);
	ts->startup_deadline_ns = 0;
	ts->power.controller_initialized = false;
	ts->epoch++;
	ts->sequence = 0;
	/* A new epoch does not grant permission for another immediate reset. */
	memset(ts->last_command_ns, 0,
	       HX_RAW_RESET * sizeof(ts->last_command_ns[0]));
	himax_state_event(ts, false);
	ts->charger.applied_valid = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
	if (ts->start_test_failures) {
		ts->start_test_failures--;
		dev_info(ts->dev, "diagnostic AFE init failure before SPI; remaining=%u\n",
			 ts->start_test_failures);
		ret = -EAGAIN;
		goto failed;
	}
#endif
	ret = hx83121a_chip_detect(ts);
	if (ret)
		goto failed;
	if (!ts->otp_tried) {
		ts->otp_tried = true;
		himax_get_project_id_otp(ts, ts->project_id,
					 sizeof(ts->project_id));
		/* OTP changes DD-register access state. */
		ret = hx83121a_chip_detect(ts);
		if (ret)
			goto failed;
	}
	/* Includes upstream preparation, power-on, verified raw selection
	 * and producer handshake. Do not follow it with legacy runtime
	 * replay: that rewrites the just-completed scan configuration.
	 * Native charger policy is applied by charger_work below.
	 */
	ret = himax_mcu_power_on_init(ts);
	if (ret)
		goto failed;
	/* Start the producer, then consume three buffered delivery attempts.
	 * IRQs/work finish cleanup without blocking this lock waiting for IRQ.
	 * The slave/pen channel remains disabled.
	 */
	himax_startup_begin(ts);
	return 0;
failed:
	dev_err(ts->dev, "raw AFE startup failed: %d\n", ret);
	return ret;
}

/* op_lock is held. Recheck gates when delayed work actually runs; never
 * keep this mutex across the Windows retry backoff or rebind the SPI parent.
 */
static void himax_start_or_retry(struct himax_ts_data *ts)
{
	unsigned int delay;
	u8 bus_value = 0;
	int ret;

	if (ts->startup.active) {
		if (himax_startup_timeout(ts))
			himax_publish_frame(ts, ts->last_frame_ns, false);
		return;
	}
	if (!hx_raw_can_start(&ts->power))
		return;
	himax_int_enable(ts, false);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
	/* The bounded synthetic failure must remain before all physical SPI. */
	if (ts->start_test_failures)
		goto attempt;
#endif
	/* Windows StepBringUp waits for a nonzero 0x13 before Init. Transport
	 * success alone does not prove panel/AFE power. Probe the finger/master
	 * channel only; never reset/rebind the parent bus to mimic handle reopen.
	 */
	ret = himax_spi_read(ts, 0x13, &bus_value, 1);
	if (ret)
		goto failed;
	if (!bus_value) {
		dev_dbg_ratelimited(ts->dev, "master bus not powered/ready; waiting 500 ms\n");
		mod_delayed_work(system_wq, &ts->panel_reinit_work, msecs_to_jiffies(500));
		return;
	}
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
attempt:
#endif
	ret = himax_start_afe(ts);
	if (!ret) {
		hx_raw_retry_reset(&ts->retry);
		return;
	}
failed:
	delay = hx_raw_retry_failed(&ts->retry);
	dev_warn_ratelimited(ts->dev, "AFE attempt %u failed (%d); retry in %u ms\n",
			     ts->retry.failures, ret, delay);
	mod_delayed_work(system_wq, &ts->panel_reinit_work, msecs_to_jiffies(delay));
}

static void himax_return_buffers(struct himax_ts_data *ts,
				 enum vb2_buffer_state state)
{
	struct himax_video_buffer *buf, *next;
	unsigned long flags;

	spin_lock_irqsave(&ts->buffer_lock, flags);
	list_for_each_entry_safe(buf, next, &ts->buffers, link) {
		list_del(&buf->link);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&ts->buffer_lock, flags);
}

/* op_lock is held. IRQ and rearm share record publication except origin.
 * SPI failures do not advance the producer or destroy its last complete data.
 */
static void himax_publish_frame(struct himax_ts_data *ts, u64 now, bool rearm)
{
	struct himax_video_buffer *buf;
	struct hx_raw_frame *frame;
	unsigned long flags;

	ts->sequence++;
	ts->last_frame_ns = now;
	spin_lock_irqsave(&ts->buffer_lock, flags);
	if (list_empty(&ts->buffers)) {
		spin_unlock_irqrestore(&ts->buffer_lock, flags);
		return; /* IRQ drains SPI even when the reader falls behind. */
	}
	buf = list_first_entry(&ts->buffers, struct himax_video_buffer, link);
	list_del(&buf->link);
	spin_unlock_irqrestore(&ts->buffer_lock, flags);
	frame = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	memset(frame, 0, sizeof(*frame));
	frame->size = sizeof(*frame);
	frame->version = HX_RAW_ABI_VERSION;
	frame->epoch = ts->epoch;
	frame->sequence = ts->sequence;
	frame->timestamp_ns = now;
	frame->flags = ts->frame_flags | (rearm ? HX_RAW_FRAME_REARM : 0) |
		(ts->charger.connected ? HX_RAW_FRAME_CHARGER : 0);
	frame->payload_size = HX_RAW_MASTER_SIZE;
	memcpy(frame->payload, ts->event_buf, sizeof(frame->payload));
	buf->vb.sequence = ts->sequence;
	buf->vb.field = V4L2_FIELD_NONE;
	buf->vb.vb2_buf.timestamp = now;
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, sizeof(*frame));
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static int himax_capture_frame(struct himax_ts_data *ts, u64 now, bool rearm)
{
	int ret;

	ret = himax_read_raw(ts);
	if (ret)
		return ret;
	ts->read_errors = 0;
	ts->last_frame_ns = now;
	if (!himax_startup_frame(ts))
		himax_publish_frame(ts, now, rearm);
	return 0;
}

/* op_lock is held, including validation and SPI. Unlike AFE commands, rearm
 * requires an exact producer sequence: an IRQ that wins this lock invalidates
 * the request rather than allowing a stale timeout to read another frame.
 */
static int himax_rearm(struct himax_ts_data *ts, struct file *file,
		       const struct hx_raw_rearm *request, bool tagged)
{
	unsigned long flags;
	bool available;
	u64 now;
	int ret;

	if (request->size != sizeof(*request) ||
	    request->version != HX_RAW_ABI_VERSION ||
	    request->reserved[0] || request->reserved[1])
		return -EINVAL;
	if (ts->power.shutting_down)
		return -ENODEV;
	if (!vb2_is_busy(&ts->queue) || ts->queue.owner != file->private_data)
		return -EBUSY;
	if (!hx_raw_can_read(&ts->power) || ts->startup.active || !request->epoch ||
	    request->epoch != ts->epoch || request->sequence != ts->sequence)
		return -ESTALE;
	now = ktime_get_ns();
	if (now - ts->last_frame_ns < 200 * NSEC_PER_MSEC ||
	    now - ts->last_poll_done_ns < 200 * NSEC_PER_MSEC ||
	    (ts->last_rearm_ns && now - ts->last_rearm_ns < 200 * NSEC_PER_MSEC))
		return -EAGAIN;
	spin_lock_irqsave(&ts->buffer_lock, flags);
	available = !list_empty(&ts->buffers);
	spin_unlock_irqrestore(&ts->buffer_lock, flags);
	if (!available)
		return -ENOBUFS;
	ts->last_rearm_ns = now;
	/* Do not initialize AFE, change epoch, or escalate a failed kick to reset.
	 * Userspace owns the bounded stall budget and verifies subsequent frames.
	 */
	ret = himax_capture_frame(ts, now, tagged);
	if (!ret)
		ts->last_poll_done_ns = ktime_get_ns();
	return ret;
}

/* op_lock held. Both legacy RESET and exact-token stalled restart use this
 * limiter and the same client-only reconstruction. The worker rechecks power
 * gates before doing SPI. Never reset or rebind the parent SPI/DMA devices.
 */
static int himax_schedule_restart(struct himax_ts_data *ts, u64 now)
{
	if (now - ts->last_command_ns[HX_RAW_RESET] < NSEC_PER_SEC)
		return -EAGAIN;
	ts->last_command_ns[HX_RAW_RESET] = now;
	himax_int_enable(ts, false);
	ts->power.controller_initialized = false;
	ts->frame_flags = HX_RAW_FRAME_SOS;
	himax_state_event(ts, false);
	mod_delayed_work(system_wq, &ts->panel_reinit_work, 0);
	return 0;
}

/* Unlike frame-bound RESET, no preceding frame is necessary. IRQ publication
 * and request validation share op_lock, so an IRQ that wins makes this token
 * stale. An accepted restart closes the read gate before releasing the lock.
 */
static int himax_restart(struct himax_ts_data *ts, struct file *file,
			 const struct hx_raw_restart *request)
{
	u64 now;

	if (request->size != sizeof(*request) ||
	    request->version != HX_RAW_ABI_VERSION ||
	    request->reserved[0] || request->reserved[1])
		return -EINVAL;
	if (ts->power.shutting_down)
		return -ENODEV;
	if (!vb2_is_busy(&ts->queue) || ts->queue.owner != file->private_data)
		return -EBUSY;
	if (!hx_raw_can_read(&ts->power) || ts->startup.active || !request->epoch ||
	    request->epoch != ts->epoch || request->sequence != ts->sequence)
		return -ESTALE;
	now = ktime_get_ns();
	if (now - ts->last_frame_ns < 200 * NSEC_PER_MSEC ||
	    now - ts->last_poll_done_ns < 200 * NSEC_PER_MSEC)
		return -EAGAIN;
	return himax_schedule_restart(ts, now);
}

static irqreturn_t himax_raw_irq(int irq, void *data)
{
	struct himax_ts_data *ts = data;
	u64 now = ktime_get_ns();
	bool starting;
	int ret;

	(void)irq;
	mutex_lock(&ts->op_lock);
	if (!hx_raw_can_read(&ts->power) || now <= ts->last_frame_ns ||
	    now <= ts->last_poll_done_ns)
		goto out;
	/* An IRQ queued before or during a successful producer poll must not
	 * reread that packet. Keep the original acquisition timestamp separate.
	 */
	starting = ts->startup.active;
	ret = himax_capture_frame(ts, now, false);
	if (ret && !starting && ++ts->read_errors >= 3) {
		himax_int_enable(ts, false);
		ts->power.controller_initialized = false;
		ts->frame_flags = HX_RAW_FRAME_SOS;
		himax_state_event(ts, false);
		mod_delayed_work(system_wq, &ts->panel_reinit_work, 0);
	}
out:
	mutex_unlock(&ts->op_lock);
	return IRQ_HANDLED;
}

static int himax_queue_setup(struct vb2_queue *q, unsigned int *count,
			     unsigned int *planes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	if (*planes)
		return *planes == 1 && sizes[0] >= sizeof(struct hx_raw_frame) ?
			0 : -EINVAL;
	/* Keep the two oldest completed packets; no free buffer means the new
	 * IRQ packet is drained but not retained. No per-frame allocation.
	 * max_num_buffers is a vb2 index-space bound, not FIFO depth: setting
	 * that below VB2_MAX_FRAME would WARN in vb2_core_queue_init().
	 */
	*count = HX_RAW_QUEUE_DEPTH;
	*planes = 1;
	sizes[0] = sizeof(struct hx_raw_frame);
	return 0;
}

static int himax_buffer_prepare(struct vb2_buffer *vb)
{
	return vb2_plane_size(vb, 0) < sizeof(struct hx_raw_frame) ? -EINVAL : 0;
}

static void himax_buffer_queue(struct vb2_buffer *vb)
{
	struct himax_ts_data *ts = vb2_get_drv_priv(vb->vb2_queue);
	struct himax_video_buffer *buf =
		container_of(to_vb2_v4l2_buffer(vb), struct himax_video_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&ts->buffer_lock, flags);
	list_add_tail(&buf->link, &ts->buffers);
	spin_unlock_irqrestore(&ts->buffer_lock, flags);
}

static int himax_stream_start(struct vb2_queue *q, unsigned int count)
{
	struct himax_ts_data *ts = vb2_get_drv_priv(q);
	int ret = 0;

	himax_lock(ts);
	if (ts->power.shutting_down) {
		ret = -ENODEV;
	} else {
		ts->power.streaming = true;
		ts->frame_flags = 0;
		hx_raw_retry_reset(&ts->retry);
		himax_start_or_retry(ts);
	}
	if (ret) {
		ts->power.streaming = false;
		himax_return_buffers(ts, VB2_BUF_STATE_QUEUED);
	}
	himax_unlock(ts);
	return ret;
}

static void himax_stream_stop(struct vb2_queue *q)
{
	struct himax_ts_data *ts = vb2_get_drv_priv(q);

	/* After unbind, the SPI and GPIO resources are no longer available. */
	if (!ts->power.shutting_down)
		himax_lock(ts);
	else
		mutex_lock(&ts->op_lock);
	ts->power.streaming = false;
	ts->power.controller_initialized = false;
	hx_raw_startup_cancel(&ts->startup);
	ts->startup_deadline_ns = 0;
	hx_raw_retry_reset(&ts->retry);
	ts->epoch++;
	himax_state_event(ts, false);
	himax_return_buffers(ts, VB2_BUF_STATE_ERROR);
	mutex_unlock(&ts->op_lock);
}

static const struct vb2_ops himax_queue_ops = {
	.queue_setup = himax_queue_setup,
	.buf_prepare = himax_buffer_prepare,
	.buf_queue = himax_buffer_queue,
	.start_streaming = himax_stream_start,
	.stop_streaming = himax_stream_stop,
};

static int himax_querycap(struct file *file, void *priv,
			  struct v4l2_capability *caps)
{
	struct himax_ts_data *ts = video_drvdata(file);

	strscpy(caps->driver, "himax-spi", sizeof(caps->driver));
	strscpy(caps->card, "Himax HX83121A raw", sizeof(caps->card));
	snprintf(caps->bus_info, sizeof(caps->bus_info), "spi:%s",
		 dev_name(ts->dev));
	return 0;
}

static int himax_enum_fmt(struct file *file, void *priv,
			  struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_META_FMT_HIMAX_SPI;
	strscpy(fmt->description, "HX83121A raw SPI frame", sizeof(fmt->description));
	return 0;
}

static int himax_meta_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	if (fmt->type != V4L2_BUF_TYPE_META_CAPTURE)
		return -EINVAL;
	memset(&fmt->fmt, 0, sizeof(fmt->fmt));
	fmt->fmt.meta.dataformat = V4L2_META_FMT_HIMAX_SPI;
	fmt->fmt.meta.buffersize = sizeof(struct hx_raw_frame);
	return 0;
}

static int himax_subscribe(struct v4l2_fh *fh,
			   const struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_HIMAX_STATE || sub->id)
		return -EINVAL;
	return v4l2_event_subscribe(fh, sub, 8, NULL);
}

static long himax_private_ioctl(struct file *file, void *priv,
				bool valid_prio, unsigned int cmd, void *arg)
{
	struct himax_ts_data *ts = video_drvdata(file);
	struct hx_raw_command *c = arg;
	u64 now;
	int ret = 0;

	mutex_lock(&ts->op_lock);
	if (ts->power.shutting_down) {
		ret = -ENODEV;
		goto out;
	}
	if (cmd == VIDIOC_HIMAX_INFO) {
		struct hx_raw_info *info = arg;

		memset(info, 0, sizeof(*info));
		info->size = sizeof(*info);
		info->version = HX_RAW_ABI_VERSION;
		info->rows = HIMAX_MAX_TX;
		info->cols = HIMAX_MAX_RX;
		info->payload_size = HX_RAW_MASTER_SIZE;
		info->coordinate_flags = ts->coordinate_flags;
		strscpy(info->project_id, ts->project_id, sizeof(info->project_id));
		goto out;
	}
	if (cmd == VIDIOC_HIMAX_REARM || cmd == VIDIOC_HIMAX_REARM_TAGGED) {
		ret = himax_rearm(ts, file, arg, cmd == VIDIOC_HIMAX_REARM_TAGGED);
		goto out;
	}
	if (cmd == VIDIOC_HIMAX_RESTART) {
		ret = himax_restart(ts, file, arg);
		goto out;
	}
	if (cmd != VIDIOC_HIMAX_COMMAND) {
		ret = -ENOTTY;
		goto out;
	}
	if (c->size != sizeof(*c) || c->version != HX_RAW_ABI_VERSION ||
	    c->reserved[0] || c->reserved[1] || c->action > HX_RAW_RESET ||
	    (c->action == HX_RAW_RESET && c->parameter)) {
		ret = -EINVAL;
		goto out;
	}
	if (!vb2_is_busy(&ts->queue) ||
	    ts->queue.owner != file->private_data) {
		ret = -EBUSY;
		goto out;
	}
	if (!hx_raw_can_read(&ts->power) || ts->startup.active ||
	    c->epoch != ts->epoch || !c->sequence || c->sequence > ts->sequence ||
	    ts->sequence - c->sequence > 8) {
		ret = -ESTALE;
		goto out;
	}
	now = ktime_get_ns();
	if (c->action == HX_RAW_RESET) {
		ret = himax_schedule_restart(ts, now);
		goto out;
	}
	if (now - ts->last_command_ns[c->action] < 20 * NSEC_PER_MSEC) {
		ret = -EAGAIN;
		goto out;
	}
	ts->last_command_ns[c->action] = now;
	ret = himax_afe_command(ts, c->action, c->parameter);
out:
	mutex_unlock(&ts->op_lock);
	return ret;
}

static const struct v4l2_ioctl_ops himax_ioctl_ops = {
	.vidioc_querycap = himax_querycap,
	.vidioc_enum_fmt_meta_cap = himax_enum_fmt,
	.vidioc_g_fmt_meta_cap = himax_meta_fmt,
	.vidioc_s_fmt_meta_cap = himax_meta_fmt,
	.vidioc_try_fmt_meta_cap = himax_meta_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = himax_subscribe,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_default = himax_private_ioctl,
};

static const struct v4l2_file_operations himax_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void himax_panel_work(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(to_delayed_work(work),
					       struct himax_ts_data, panel_reinit_work);

	mutex_lock(&ts->op_lock);
	/* The IRQ thread also needs op_lock. The helper only disables IRQ
	 * without synchronizing it, and leaves a healthy stream alone.
	 */
	himax_start_or_retry(ts);
	himax_unlock(ts);
}

static void himax_charger_work(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(to_delayed_work(work),
					       struct himax_ts_data, charger_work);
	int supplied;
	int ret;

	mutex_lock(&ts->op_lock);
	if (!hx_raw_can_read(&ts->power) || ts->startup.active)
		goto out;
	supplied = power_supply_is_system_supplied();
	if (supplied < 0)
		goto retry;
	ts->charger.connected = supplied > 0;
	if (!hx_raw_charger_needs_write(&ts->charger))
		goto out;
	ret = himax_set_charger(ts, ts->charger.connected);
	if (ret) {
		dev_warn_ratelimited(ts->dev, "charger update: %d\n", ret);
		goto retry;
	}
	goto out;
retry:
	/* Retry a failed query/write without requiring another power event.
	 * Suspend, panel-off and remove close the gate before draining work.
	 */
	mod_delayed_work(system_wq, &ts->charger_work, msecs_to_jiffies(1000));
out:
	mutex_unlock(&ts->op_lock);
}

static int himax_charger_notify(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct himax_ts_data *ts = container_of(nb, struct himax_ts_data,
					       charger_notifier);

	if (event == PSY_EVENT_PROP_CHANGED)
		mod_delayed_work(system_wq, &ts->charger_work, 0);
	return NOTIFY_OK;
}

static int himax_panel_prepared(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
					       panel_follower);

	mutex_lock(&ts->op_lock);
	ts->power.panel_prepared = true;
	mutex_unlock(&ts->op_lock);
	return 0;
}

static int himax_panel_enabled(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
					       panel_follower);

	mutex_lock(&ts->op_lock);
	ts->power.panel_enabled = true;
	ts->frame_flags = 0;
	if (hx_raw_can_start(&ts->power))
		mod_delayed_work(system_wq, &ts->panel_reinit_work, msecs_to_jiffies(300));
	mutex_unlock(&ts->op_lock);
	return 0;
}

static int himax_panel_disabling(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
					       panel_follower);

	himax_lock(ts);
	ts->power.panel_enabled = false;
	ts->power.controller_initialized = false;
	hx_raw_startup_cancel(&ts->startup);
	ts->startup_deadline_ns = 0;
	hx_raw_retry_reset(&ts->retry);
	ts->epoch++;
	himax_state_event(ts, false);
	himax_unlock(ts);
	/* Publish the gate first. Never wait for a worker while holding op_lock. */
	cancel_delayed_work_sync(&ts->panel_reinit_work);
	cancel_delayed_work_sync(&ts->charger_work);
	return 0;
}

static int himax_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct himax_ts_data *ts = container_of(follower, struct himax_ts_data,
					       panel_follower);

	himax_panel_disabling(follower);
	mutex_lock(&ts->op_lock);
	ts->power.panel_prepared = false;
	mutex_unlock(&ts->op_lock);
	return 0;
}

static const struct drm_panel_follower_funcs himax_panel_ops = {
	.panel_prepared = himax_panel_prepared,
	.panel_enabled = himax_panel_enabled,
	.panel_disabling = himax_panel_disabling,
	.panel_unpreparing = himax_panel_unpreparing,
};

static int himax_raw_suspend(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	himax_lock(ts);
	ts->power.system_suspended = true;
	ts->power.controller_initialized = false;
	hx_raw_startup_cancel(&ts->startup);
	ts->startup_deadline_ns = 0;
	hx_raw_retry_reset(&ts->retry);
	ts->epoch++;
	himax_state_event(ts, false);
	himax_unlock(ts);
	cancel_delayed_work_sync(&ts->panel_reinit_work);
	cancel_delayed_work_sync(&ts->charger_work);
	/* Match Chip::Quiesce: no sense-off, reset GPIO or SPI during stop. */
	return 0;
}

static int himax_raw_resume(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	mutex_lock(&ts->op_lock);
	ts->power.system_suspended = false;
	/* The SPI parent has resumed. Panel callbacks may precede or follow us;
	 * whichever opens the last gate queues a full AFE initialization.
	 */
	if (hx_raw_can_start(&ts->power))
		mod_delayed_work(system_wq, &ts->panel_reinit_work, msecs_to_jiffies(300));
	mutex_unlock(&ts->op_lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(himax_raw_pm_ops, himax_raw_suspend, himax_raw_resume);

static void himax_raw_shutdown(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	himax_lock(ts);
	ts->power.shutting_down = true;
	ts->power.controller_initialized = false;
	hx_raw_startup_cancel(&ts->startup);
	ts->startup_deadline_ns = 0;
	hx_raw_retry_reset(&ts->retry);
	ts->epoch++;
	himax_state_event(ts, false);
	himax_unlock(ts);
	cancel_delayed_work_sync(&ts->panel_reinit_work);
	cancel_delayed_work_sync(&ts->charger_work);
}

static void himax_v4l2_release(struct v4l2_device *v4l2)
{
	struct himax_ts_data *ts = container_of(v4l2, struct himax_ts_data, v4l2);

	v4l2_device_unregister(v4l2);
	kfree(ts->event_buf);
	kfree(ts->xfer_buf);
	kfree(ts);
}

static int himax_raw_probe(struct spi_device *spi)
{
	struct himax_ts_data *ts;
	struct vb2_queue *q;
	int ret;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
	if (himax_afe_start_failures > 8)
		return dev_err_probe(&spi->dev, -EINVAL,
				     "diagnostic afe_start_failures must be 0..8\n");
#endif
	ts = kzalloc_obj(*ts, GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->dev = &spi->dev;
	ts->spi = spi;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS
	ts->start_test_failures = himax_afe_start_failures;
#endif
	if (device_property_read_bool(ts->dev, "touchscreen-inverted-x"))
		ts->coordinate_flags |= HX_RAW_INVERT_X;
	if (device_property_read_bool(ts->dev, "touchscreen-inverted-y"))
		ts->coordinate_flags |= HX_RAW_INVERT_Y;
	if (device_property_read_bool(ts->dev, "touchscreen-swapped-x-y"))
		ts->coordinate_flags |= HX_RAW_SWAP_XY;
	ts->event_buf_sz = HIMAX_FULL_EVENT_STACK_SIZE;
	ts->xfer_buf_sz = HX_RAW_MASTER_SIZE + HIMAX_BUS_READ_HEADER_SIZE;
	if (spi_max_transfer_size(spi) < ts->xfer_buf_sz ||
	    spi_max_message_size(spi) < ts->xfer_buf_sz) {
		ret = dev_err_probe(ts->dev, -EMSGSIZE,
				   "SPI controller cannot read a complete master frame\n");
		goto free_ts;
	}
	ts->xfer_buf = kmalloc(ts->xfer_buf_sz, GFP_KERNEL);
	ts->event_buf = kmalloc(ts->event_buf_sz, GFP_KERNEL);
	if (!ts->xfer_buf || !ts->event_buf) {
		ret = -ENOMEM;
		goto free_ts;
	}
	/* The Windows reset protocol starts with a register write. Do not hold
	 * RESET asserted while acquiring the GPIO, or that first write cannot
	 * reach the IC. Native low/released retains the existing ACPI polarity.
	 */
	ts->gpiod_rst = devm_gpiod_get(ts->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->gpiod_rst)) {
		ret = PTR_ERR(ts->gpiod_rst);
		goto free_ts;
	}
	usleep_range(50000, 50100);
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3;
	spi->cs_setup.value = HIMAX_SPI_CS_SETUP_TIME;
	ret = spi_setup(spi);
	if (ret)
		goto free_ts;
	spin_lock_init(&ts->irq_lock);
	spin_lock_init(&ts->buffer_lock);
	mutex_init(&ts->op_lock);
	mutex_init(&ts->video_lock);
	INIT_LIST_HEAD(&ts->buffers);
	INIT_DELAYED_WORK(&ts->panel_reinit_work, himax_panel_work);
	INIT_DELAYED_WORK(&ts->charger_work, himax_charger_work);
	ts->charger_notifier.notifier_call = himax_charger_notify;
	ret = v4l2_device_register(ts->dev, &ts->v4l2);
	if (ret)
		goto free_ts;
	ts->v4l2.release = himax_v4l2_release;
	spi_set_drvdata(spi, ts); /* v4l2_device_register sets parent drvdata. */
	q = &ts->queue;
	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_READ;
	q->drv_priv = ts;
	q->buf_struct_size = sizeof(struct himax_video_buffer);
	q->ops = &himax_queue_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 1;
	q->lock = &ts->video_lock;
	q->dev = ts->dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto put_v4l2;
	ts->video = video_device_alloc();
	if (!ts->video) {
		ret = -ENOMEM;
		goto put_v4l2;
	}
	strscpy(ts->video->name, "Himax HX83121A raw", sizeof(ts->video->name));
	ts->video->v4l2_dev = &ts->v4l2;
	ts->video->fops = &himax_video_fops;
	ts->video->ioctl_ops = &himax_ioctl_ops;
	ts->video->release = video_device_release;
	ts->video->lock = &ts->video_lock;
	ts->video->queue = q;
	ts->video->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING |
		V4L2_CAP_READWRITE;
	video_set_drvdata(ts->video, ts);
	ret = request_threaded_irq(spi->irq, NULL, himax_raw_irq,
				  IRQF_ONESHOT | IRQF_NO_AUTOEN,
				  "himax-raw", ts);
	if (ret)
		goto free_video;
	ret = video_register_device(ts->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto free_irq;
	ret = power_supply_reg_notifier(&ts->charger_notifier);
	if (ret)
		goto unregister_video;
	ts->panel_follower.funcs = &himax_panel_ops;
	ret = drm_panel_add_follower(ts->dev, &ts->panel_follower);
	if (ret) {
		power_supply_unreg_notifier(&ts->charger_notifier);
		himax_lock(ts);
		ts->power.shutting_down = true;
		himax_unlock(ts);
		cancel_delayed_work_sync(&ts->charger_work);
		cancel_delayed_work_sync(&ts->panel_reinit_work);
		goto unregister_video;
	}
	dev_info(ts->dev, "raw SPI capture registered as %s\n",
		 video_device_node_name(ts->video));
	return 0;
unregister_video:
	free_irq(spi->irq, ts);
	vb2_video_unregister_device(ts->video);
	goto put_v4l2;
free_irq:
	free_irq(spi->irq, ts);
free_video:
	video_device_release(ts->video);
put_v4l2:
	v4l2_device_put(&ts->v4l2);
	return ret;
free_ts:
	kfree(ts->event_buf);
	kfree(ts->xfer_buf);
	kfree(ts);
	return ret;
}

static void himax_raw_remove(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	himax_raw_shutdown(spi);
	drm_panel_remove_follower(&ts->panel_follower);
	power_supply_unreg_notifier(&ts->charger_notifier);
	cancel_delayed_work_sync(&ts->charger_work);
	cancel_delayed_work_sync(&ts->panel_reinit_work);
	mutex_lock(&ts->video_lock);
	himax_lock(ts);
	ts->power.shutting_down = true;
	ts->power.controller_initialized = false;
	himax_state_event(ts, false);
	himax_return_buffers(ts, VB2_BUF_STATE_ERROR);
	vb2_queue_error(&ts->queue);
	himax_unlock(ts);
	v4l2_device_disconnect(&ts->v4l2);
	mutex_unlock(&ts->video_lock);
	free_irq(spi->irq, ts);
	vb2_video_unregister_device(ts->video);
	v4l2_device_put(&ts->v4l2);
}

static const struct spi_device_id himax_raw_ids[] = {
	{ "hx83121a-ts", 0 },
	{ "hx83121a", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, himax_raw_ids);

static const struct of_device_id himax_raw_of_match[] = {
	{ .compatible = "himax,hx83121a" },
	{ }
};
MODULE_DEVICE_TABLE(of, himax_raw_of_match);

static struct spi_driver himax_raw_driver = {
	.driver = {
		.name = "himax-spi",
		.of_match_table = himax_raw_of_match,
		.pm = pm_sleep_ptr(&himax_raw_pm_ops),
	},
	.probe = himax_raw_probe,
	.remove = himax_raw_remove,
	.shutdown = himax_raw_shutdown,
	.id_table = himax_raw_ids,
};
module_spi_driver(himax_raw_driver);

MODULE_DESCRIPTION("HX83121A raw SPI to V4L2 metadata transport");
MODULE_AUTHOR("EGoTouchRev Linux contributors");
MODULE_LICENSE("GPL");
