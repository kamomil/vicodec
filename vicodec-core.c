// SPDX-License-Identifier: GPL-2.0-only
/*
 * A virtual codec example device.
 *
 * Copyright 2018 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *
 * This is a virtual codec device driver for testing the codec framework.
 * It simulates a device that uses memory buffers for both source and
 * destination and encodes or decodes the data.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <linux/platform_device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>

#include "codec-v4l2-fwht.h"

MODULE_DESCRIPTION("Virtual codec device");
MODULE_AUTHOR("Hans Verkuil <hans.verkuil@cisco.com>");
MODULE_LICENSE("GPL v2");

static bool multiplanar;
module_param(multiplanar, bool, 0444);
MODULE_PARM_DESC(multiplanar,
		 " use multi-planar API instead of single-planar API");

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, " activates debug info");

#define VICODEC_NAME		"vicodec"
#define MAX_WIDTH		4096U
#define MIN_WIDTH		640U
#define MAX_HEIGHT		2160U
#define MIN_HEIGHT		360U

#define dprintk(dev, fmt, arg...) \
	v4l2_dbg(1, debug, &dev->v4l2_dev, "%s: " fmt, __func__, ## arg)


struct pixfmt_info {
	u32 id;
	unsigned int bytesperline_mult;
	unsigned int sizeimage_mult;
	unsigned int sizeimage_div;
	unsigned int luma_step;
	unsigned int chroma_step;
	/* Chroma plane subsampling */
	unsigned int width_div;
	unsigned int height_div;
};

static const struct v4l2_fwht_pixfmt_info pixfmt_fwht = {
	V4L2_PIX_FMT_FWHT, 0, 3, 1, 1, 1, 1, 1, 0, 1
};

static const struct v4l2_fwht_pixfmt_info pixfmt_stateless_fwht = {
	V4L2_PIX_FMT_FWHT_STATELESS, 0, 3, 1, 1, 1, 1, 1, 0, 1
};

static void vicodec_dev_release(struct device *dev)
{
}

static struct platform_device vicodec_pdev = {
	.name		= VICODEC_NAME,
	.dev.release	= vicodec_dev_release,
};

/* Per-queue, driver-specific private data */
struct vicodec_q_data {
	unsigned int		coded_width;
	unsigned int		coded_height;
	unsigned int		visible_width;
	unsigned int		visible_height;
	unsigned int		sizeimage;
	unsigned int		sequence;
	const struct v4l2_fwht_pixfmt_info *info;
};

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

struct vicodec_dev_instance {
	struct video_device     vfd;
	struct mutex            mutex;
	spinlock_t              lock;
	struct v4l2_m2m_dev     *m2m_dev;
};

struct vicodec_dev {
	struct v4l2_device	v4l2_dev;
	struct vicodec_dev_instance stateful_enc;
	struct vicodec_dev_instance stateful_dec;
	struct vicodec_dev_instance stateless_dec;
#ifdef CONFIG_MEDIA_CONTROLLER
	struct media_device	mdev;
#endif

};

struct vicodec_ctx {
	struct v4l2_fh		fh;
	struct vicodec_dev	*dev;
	bool			is_enc;
	bool			is_stateless;
	spinlock_t		*lock;
	struct media_request	*cur_req;

	struct v4l2_ctrl_handler hdl;

	struct vb2_v4l2_buffer *last_src_buf;
	struct vb2_v4l2_buffer *last_dst_buf;

	/* Source and destination queue data */
	struct vicodec_q_data   q_data[2];
	struct v4l2_fwht_state	state;

	u32			cur_buf_offset;
	u32			comp_max_size;
	u32			comp_size;
	u32			header_size;
	u32			comp_magic_cnt;
	bool			comp_has_frame;
	bool			comp_has_next_frame;
	bool			first_source_change_sent;
	bool			source_changed;
	bool			bad_statless_params;
};

static inline struct vicodec_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct vicodec_ctx, fh);
}

static struct vicodec_q_data *get_q_data(struct vicodec_ctx *ctx,
					 enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return &ctx->q_data[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return &ctx->q_data[V4L2_M2M_DST];
	default:
		WARN_ON(1);
		break;
	}
	return NULL;
}

static int device_process(struct vicodec_ctx *ctx,
			  struct vb2_v4l2_buffer *src_vb,
			  struct vb2_v4l2_buffer *dst_vb)
{
	struct vicodec_dev *dev = ctx->dev;
	struct vicodec_q_data *q_dst;
	struct v4l2_fwht_state *state = &ctx->state;
	u8 *p_src, *p_dst;
	int ret;

	q_dst = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (ctx->is_enc) {
		p_src = vb2_plane_vaddr(&src_vb->vb2_buf, 0);
	}
	else {
		p_src = state->compressed_frame;
	}
	if (ctx->is_stateless) {
		struct media_request *src_req;
		/* Apply request(s) controls if needed. */
		src_req = src_vb->vb2_buf.req_obj.req;

		if (!src_req) {
			v4l2_err(&dev->v4l2_dev, "%s: src_req is NULL\n", __func__);
			return -EFAULT;
		}
		ctx->cur_req = src_req;
		v4l2_ctrl_request_setup(src_req, &ctx->hdl);
		if (ctx->bad_statless_params) {
			pr_info("%s: bad stateless params\n", __func__);
			return -EINVAL;
		}
	}
	p_dst = vb2_plane_vaddr(&dst_vb->vb2_buf, 0);
	pr_info("dafna: %s dst is %p, size is %lu\n",__func__,p_dst, vb2_plane_size(&dst_vb->vb2_buf, 0));
	if (!p_src || !p_dst) {
		v4l2_err(&dev->v4l2_dev,
			 "Acquiring kernel pointers to buffers failed\n");
		return -EFAULT;
	}

	if (ctx->is_enc) {
		struct vicodec_q_data *q_src;

		q_src = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		state->info = q_src->info;
		pr_info("dafna: %s: aboout to call v4l2_fwht_encode\n", __func__);
		ret = v4l2_fwht_encode(state, p_src, p_dst);
		if (ret < 0)
			return ret;
		vb2_set_plane_payload(&dst_vb->vb2_buf, 0, ret);
	} else {
		unsigned int comp_frame_size = ntohl(ctx->state.header.size);

		if (comp_frame_size > ctx->comp_max_size)
			return -EINVAL;
		state->info = q_dst->info;
		pr_info("dafna: %s: aboout to call v4l2_fwht_decode\n", __func__);
		ret = v4l2_fwht_decode(state, p_src, p_dst);
		if (ret < 0)
			return ret;
		if (!ctx->is_stateless)
			copy_cap_to_ref(p_dst, ctx->state.info, &ctx->state);

		vb2_set_plane_payload(&dst_vb->vb2_buf, 0, q_dst->sizeimage);
	}

	dst_vb->sequence = q_dst->sequence++;
	dst_vb->flags &= ~V4L2_BUF_FLAG_LAST;
	v4l2_m2m_buf_copy_data(src_vb, dst_vb, !ctx->is_enc);

	return 0;
}

/*
 * mem2mem callbacks
 */
static enum vb2_buffer_state get_next_header(struct vicodec_ctx *ctx,
					     u8 **pp, u32 sz)
{
	static const u8 magic[] = {
		0x4f, 0x4f, 0x4f, 0x4f, 0xff, 0xff, 0xff, 0xff
	};
	u8 *p = *pp;
	u32 state;
	u8 *header = (u8 *)&ctx->state.header;

	state = VB2_BUF_STATE_DONE;

	if (!ctx->header_size) {
		state = VB2_BUF_STATE_ERROR;
		for (; p < *pp + sz; p++) {
			u32 copy;

			p = memchr(p, magic[ctx->comp_magic_cnt],
				   *pp + sz - p);
			if (!p) {
				ctx->comp_magic_cnt = 0;
				p = *pp + sz;
				break;
			}
			copy = sizeof(magic) - ctx->comp_magic_cnt;
			if (*pp + sz - p < copy)
				copy = *pp + sz - p;

			memcpy(header + ctx->comp_magic_cnt, p, copy);
			ctx->comp_magic_cnt += copy;
			if (!memcmp(header, magic, ctx->comp_magic_cnt)) {
				p += copy;
				state = VB2_BUF_STATE_DONE;
				break;
			}
			ctx->comp_magic_cnt = 0;
		}
		if (ctx->comp_magic_cnt < sizeof(magic)) {
			*pp = p;
			return state;
		}
		ctx->header_size = sizeof(magic);
	}

	if (ctx->header_size < sizeof(struct fwht_cframe_hdr)) {
		u32 copy = sizeof(struct fwht_cframe_hdr) - ctx->header_size;

		if (*pp + sz - p < copy)
			copy = *pp + sz - p;

		memcpy(header + ctx->header_size, p, copy);
		p += copy;
		ctx->header_size += copy;
	}
	*pp = p;
	return state;
}

/* device_run() - prepares and starts the device */
static void device_run(void *priv)
{
	static const struct v4l2_event eos_event = {
		.type = V4L2_EVENT_EOS
	};
	struct vicodec_ctx *ctx = priv;
	struct vicodec_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct vicodec_q_data *q_src;
	u32 state;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	pr_info("dafna: %s ctx = %p src = %p, dst = %p\n",__func__, ctx, src_buf, dst_buf);
	q_src = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);

	state = VB2_BUF_STATE_DONE;
	pr_info("dafna: %s: about to call device_process\n",__func__);
	if (device_process(ctx, src_buf, dst_buf))
		state = VB2_BUF_STATE_ERROR;
	ctx->last_dst_buf = dst_buf;

	spin_lock(ctx->lock);
	if (!ctx->comp_has_next_frame && src_buf == ctx->last_src_buf) {
		pr_info("%s: dafna: setting buffer to be last\n", __func__);
		dst_buf->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &eos_event);
	}
	if (ctx->is_enc || ctx->is_stateless) {
		src_buf->sequence = q_src->sequence++;
		src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_buf_done(src_buf, state);
	} else if (vb2_get_plane_payload(&src_buf->vb2_buf, 0) == ctx->cur_buf_offset) {
		src_buf->sequence = q_src->sequence++;
		src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_buf_done(src_buf, state);
		ctx->cur_buf_offset = 0;
		ctx->comp_has_next_frame = false;
	}
	v4l2_m2m_buf_done(dst_buf, state);
	if (ctx->is_stateless) {
		struct media_request *src_req;
		src_req = src_buf->vb2_buf.req_obj.req;
		if (src_req)
			v4l2_ctrl_request_complete(src_req, &ctx->hdl);
	}
	ctx->comp_size = 0;
	ctx->header_size = 0;
	ctx->comp_magic_cnt = 0;
	ctx->comp_has_frame = false;
	spin_unlock(ctx->lock);

	if (ctx->is_enc)
		v4l2_m2m_job_finish(dev->stateful_enc.m2m_dev, ctx->fh.m2m_ctx);
	else if (ctx->is_stateless)
		v4l2_m2m_job_finish(dev->stateless_dec.m2m_dev,
				    ctx->fh.m2m_ctx);
	else
		v4l2_m2m_job_finish(dev->stateful_dec.m2m_dev, ctx->fh.m2m_ctx);
}

static void job_remove_src_buf(struct vicodec_ctx *ctx, u32 state)
{
	struct vb2_v4l2_buffer *src_buf;
	struct vicodec_q_data *q_src;

	q_src = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	spin_lock(ctx->lock);
	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	src_buf->sequence = q_src->sequence++;
	v4l2_m2m_buf_done(src_buf, state);
	ctx->cur_buf_offset = 0;
	spin_unlock(ctx->lock);
}

static const struct v4l2_fwht_pixfmt_info *
info_from_header(const struct fwht_cframe_hdr *p_hdr)
{
	unsigned int flags = ntohl(p_hdr->flags);
	unsigned int width_div = (flags & FWHT_FL_CHROMA_FULL_WIDTH) ? 1 : 2;
	unsigned int height_div = (flags & FWHT_FL_CHROMA_FULL_HEIGHT) ? 1 : 2;
	unsigned int components_num = 3;
	unsigned int pixenc = 0;
	unsigned int version = ntohl(p_hdr->version);

	if (version == FWHT_VERSION) {
		components_num = 1 + ((flags & FWHT_FL_COMPONENTS_NUM_MSK) >>
				FWHT_FL_COMPONENTS_NUM_OFFSET);
		pixenc = (flags & FWHT_FL_PIXENC_MSK);
	}
	return v4l2_fwht_default_fmt(width_div, height_div,
				     components_num, pixenc, 0);
}

static bool is_header_valid(const struct fwht_cframe_hdr *p_hdr)
{
	const struct v4l2_fwht_pixfmt_info *info;
	unsigned int w = ntohl(p_hdr->width);
	unsigned int h = ntohl(p_hdr->height);
	unsigned int version = ntohl(p_hdr->version);
	unsigned int flags = ntohl(p_hdr->flags);

	if (!version || version > FWHT_VERSION)
		return false;

	if (w < MIN_WIDTH || w > MAX_WIDTH || h < MIN_HEIGHT || h > MAX_HEIGHT)
		return false;

	if (version == FWHT_VERSION) {
		unsigned int components_num = 1 +
			((flags & FWHT_FL_COMPONENTS_NUM_MSK) >>
			FWHT_FL_COMPONENTS_NUM_OFFSET);
		unsigned int pixenc = flags & FWHT_FL_PIXENC_MSK;

		if (components_num == 0 || components_num > 4 || !pixenc)
			return false;
	}

	info = info_from_header(p_hdr);
	if (!info)
		return false;
	return true;
}

static void update_capture_data_from_header(struct vicodec_ctx *ctx)
{
	struct vicodec_q_data *q_dst = get_q_data(ctx,
						  V4L2_BUF_TYPE_VIDEO_CAPTURE);
	const struct fwht_cframe_hdr *p_hdr = &ctx->state.header;
	const struct v4l2_fwht_pixfmt_info *info = info_from_header(p_hdr);
	unsigned int flags = ntohl(p_hdr->flags);
	unsigned int hdr_width_div = (flags & FWHT_FL_CHROMA_FULL_WIDTH) ? 1 : 2;
	unsigned int hdr_height_div = (flags & FWHT_FL_CHROMA_FULL_HEIGHT) ? 1 : 2;

	q_dst->info = info;
	q_dst->visible_width = ntohl(p_hdr->width);
	q_dst->visible_height = ntohl(p_hdr->height);
	q_dst->coded_width = vic_round_dim(q_dst->visible_width, hdr_width_div);
	q_dst->coded_height = vic_round_dim(q_dst->visible_height,
					    hdr_height_div);

	q_dst->sizeimage = q_dst->coded_width * q_dst->coded_height *
		q_dst->info->sizeimage_mult / q_dst->info->sizeimage_div;
	ctx->state.colorspace = ntohl(p_hdr->colorspace);

	ctx->state.xfer_func = ntohl(p_hdr->xfer_func);
	ctx->state.ycbcr_enc = ntohl(p_hdr->ycbcr_enc);
	ctx->state.quantization = ntohl(p_hdr->quantization);
	pr_info("%s: dim %ux%u div %ux%u\n",__func__,
			q_dst->visible_width,
			q_dst->visible_height, hdr_width_div, hdr_height_div);
	pr_info("%s: %u\n",__func__, info->id);
	pr_info("%s: dafna format is '%s'\n",__func__,id_fmt_to_str(info->id));
}

static void set_last_buffer(struct vb2_v4l2_buffer *dst_buf,
			    const struct vb2_v4l2_buffer *src_buf,
			    struct vicodec_ctx *ctx)
{
	struct vicodec_q_data *q_dst = get_q_data(ctx,
						  V4L2_BUF_TYPE_VIDEO_CAPTURE);

	pr_info("%s: setting last buffer %p (%p)\n", __func__, dst_buf, &dst_buf->vb2_buf);
	vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
	dst_buf->sequence = q_dst->sequence++;

	v4l2_m2m_buf_copy_data(src_buf, dst_buf, !ctx->is_enc);
	dst_buf->flags |= V4L2_BUF_FLAG_LAST;
	v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);
}

static int job_ready(void *priv)
{
	static const u8 magic[] = {
		0x4f, 0x4f, 0x4f, 0x4f, 0xff, 0xff, 0xff, 0xff
	};
	struct vicodec_ctx *ctx = priv;
	struct vb2_v4l2_buffer *src_buf;
	u8 *p_src;
	u8 *p;
	u32 sz;
	u32 state;
	struct vicodec_q_data *q_dst = get_q_data(ctx,
						  V4L2_BUF_TYPE_VIDEO_CAPTURE);
	unsigned int flags;
	unsigned int hdr_width_div;
	unsigned int hdr_height_div;
	unsigned int max_to_copy;
	unsigned int comp_frame_size;

	if (ctx->source_changed)
		return 0;
	if (ctx->is_stateless || ctx->is_enc || ctx->comp_has_frame)
		return 1;

restart:
	ctx->comp_has_next_frame = false;
	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!src_buf)
		return 0;
	p_src = vb2_plane_vaddr(&src_buf->vb2_buf, 0);
	sz = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	p = p_src + ctx->cur_buf_offset;

	state = VB2_BUF_STATE_DONE;

	if (ctx->header_size < sizeof(struct fwht_cframe_hdr)) {
		state = get_next_header(ctx, &p, p_src + sz - p);
		if (ctx->header_size < sizeof(struct fwht_cframe_hdr)) {
			job_remove_src_buf(ctx, state);
			goto restart;
		}
	}

	comp_frame_size = ntohl(ctx->state.header.size);
	pr_info("dafna: %s size in header is %u\n", __func__, comp_frame_size);

	/*
	 * The current scanned frame might be the first frame of a new
	 * resolution so its size might be larger than ctx->comp_max_size.
	 * In that case it is copied up to the current buffer capacity and
	 * the copy will continue after allocating new large enough buffer
	 * when restreaming
	 */
	max_to_copy = min(comp_frame_size, ctx->comp_max_size);

	if (ctx->comp_size < max_to_copy) {
		u32 copy = max_to_copy - ctx->comp_size;

		if (copy > p_src + sz - p)
			copy = p_src + sz - p;
		pr_info("dafna: %s copying AGAIN AGAIN %u to compressed_frame\n", __func__,copy);

		memcpy(ctx->state.compressed_frame + ctx->comp_size,
		       p, copy);
		p += copy;
		ctx->comp_size += copy;
		if (ctx->comp_size < max_to_copy) {
			job_remove_src_buf(ctx, state);
			goto restart;
		}
	}
	ctx->cur_buf_offset = p - p_src;
	if (ctx->comp_size == comp_frame_size)
		ctx->comp_has_frame = true;
	ctx->comp_has_next_frame = false;
	if (ctx->comp_has_frame && sz - ctx->cur_buf_offset >=
			sizeof(struct fwht_cframe_hdr)) {
		struct fwht_cframe_hdr *p_hdr = (struct fwht_cframe_hdr *)p;
		u32 frame_size = ntohl(p_hdr->size);
		u32 remaining = sz - ctx->cur_buf_offset - sizeof(*p_hdr);

		if (!memcmp(p, magic, sizeof(magic)))
			ctx->comp_has_next_frame = remaining >= frame_size;
	}
	/*
	 * if the header is invalid the device_run will just drop the frame
	 * with an error
	 */
	if (!is_header_valid(&ctx->state.header) && ctx->comp_has_frame)
		return 1;
	flags = ntohl(ctx->state.header.flags);
	hdr_width_div = (flags & FWHT_FL_CHROMA_FULL_WIDTH) ? 1 : 2;
	hdr_height_div = (flags & FWHT_FL_CHROMA_FULL_HEIGHT) ? 1 : 2;

	pr_info("%s: flags is 0x%x div %ux%u\n", __func__,flags, hdr_width_div, hdr_height_div);
	if (ntohl(ctx->state.header.width) != q_dst->visible_width ||
	    ntohl(ctx->state.header.height) != q_dst->visible_height ||
	    !q_dst->info ||
	    hdr_width_div != q_dst->info->width_div ||
	    hdr_height_div != q_dst->info->height_div) {
		static const struct v4l2_event rs_event = {
			.type = V4L2_EVENT_SOURCE_CHANGE,
			.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
		};

		struct vb2_v4l2_buffer *dst_buf =
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		pr_info("%s: SOURCE_CHANGE detected\n", __func__);
		update_capture_data_from_header(ctx);
		ctx->first_source_change_sent = true;
		v4l2_event_queue_fh(&ctx->fh, &rs_event);
		set_last_buffer(dst_buf, src_buf, ctx);
		ctx->source_changed = true;
		return 0;
	}
	return 1;
}

/*
 * video ioctls
 */

static const struct v4l2_fwht_pixfmt_info *find_fmt(u32 fmt)
{
	const struct v4l2_fwht_pixfmt_info *info =
		v4l2_fwht_find_pixfmt(fmt);

	if (!info)
		info = v4l2_fwht_get_pixfmt(0);
	return info;
}

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, VICODEC_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, VICODEC_NAME, sizeof(cap->card) - 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info),
			"platform:%s", VICODEC_NAME);
	return 0;
}

static int enum_fmt(struct v4l2_fmtdesc *f, struct vicodec_ctx *ctx,
		    bool is_out)
{
	bool is_uncomp = (ctx->is_enc && is_out) || (!ctx->is_enc && !is_out);

	pr_info("enum_fmt '%s' is_out = %d\n", V4L2_TYPE_IS_OUTPUT(f->type) ? "OUT" : "CAP", is_out);
	if (V4L2_TYPE_IS_MULTIPLANAR(f->type) && !multiplanar)
		return -EINVAL;
	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type) && multiplanar)
		return -EINVAL;

	if (is_uncomp) {
		const struct v4l2_fwht_pixfmt_info *info =
					get_q_data(ctx, f->type)->info;

		if (!info || ctx->is_enc)
			info = v4l2_fwht_get_pixfmt(f->index);
		else
			info = v4l2_fwht_default_fmt(info->width_div,
						     info->height_div,
						     info->components_num,
						     info->pixenc,
						     f->index);
		if (!info)
			return -EINVAL;
		f->pixelformat = info->id;
	} else {
		if (f->index)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_FWHT;
	}
	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct vicodec_ctx *ctx = file2ctx(file);

	return enum_fmt(f, ctx, false);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct vicodec_ctx *ctx = file2ctx(file);

	return enum_fmt(f, ctx, true);
}

static int vidioc_g_fmt(struct vicodec_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct vicodec_q_data *q_data;
	struct v4l2_pix_format_mplane *pix_mp;
	struct v4l2_pix_format *pix;
	const struct v4l2_fwht_pixfmt_info *info;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	info = q_data->info;

	if (!info)
		info = v4l2_fwht_get_pixfmt(0);
	pr_info("dafna: vidioc_g_fmt for type '%s'\n", V4L2_TYPE_IS_OUTPUT(f->type) ? "OUT" : "CAP");

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		if (multiplanar)
			return -EINVAL;
		pix = &f->fmt.pix;
		pix->width = q_data->coded_width;
		pix->height = q_data->coded_height;
		pix->field = V4L2_FIELD_NONE;
		pix->pixelformat = info->id;
		pix->bytesperline = q_data->coded_width *
					info->bytesperline_mult;
		pr_info("dafna: %s: bytesperline(%u) = coded_width(%u) * bytesperline_mult (%u)\n",__func__,
				pix->bytesperline,q_data->coded_width,info ? info->bytesperline_mult : 0);
		pix->sizeimage = q_data->sizeimage;
		pr_info("dafna: %s: sizeimage = %u\n",__func__,pix->sizeimage);
		pix->colorspace = ctx->state.colorspace;
		pix->xfer_func = ctx->state.xfer_func;
		pix->ycbcr_enc = ctx->state.ycbcr_enc;
		pix->quantization = ctx->state.quantization;
		break;

	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		if (!multiplanar)
			return -EINVAL;
		pix_mp = &f->fmt.pix_mp;
		pix_mp->width = q_data->coded_width;
		pix_mp->height = q_data->coded_height;
		pix_mp->field = V4L2_FIELD_NONE;
		pix_mp->pixelformat = info->id;
		pix_mp->num_planes = 1;
		pix_mp->plane_fmt[0].bytesperline =
				q_data->coded_width * info->bytesperline_mult;
		pr_info("%s: bytesperline(%u) = coded_width(%u) * bytesperline_mult (%u)\n",__func__,
				pix_mp->plane_fmt[0].bytesperline,q_data->coded_width,info ? info->bytesperline_mult : 0);
		pix_mp->plane_fmt[0].sizeimage = q_data->sizeimage;
		pix_mp->colorspace = ctx->state.colorspace;
		pix_mp->xfer_func = ctx->state.xfer_func;
		pix_mp->ycbcr_enc = ctx->state.ycbcr_enc;
		pix_mp->quantization = ctx->state.quantization;
		memset(pix_mp->reserved, 0, sizeof(pix_mp->reserved));
		memset(pix_mp->plane_fmt[0].reserved, 0,
		       sizeof(pix_mp->plane_fmt[0].reserved));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int vidioc_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_try_fmt(struct vicodec_ctx *ctx, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp;
	struct v4l2_pix_format *pix;
	struct v4l2_plane_pix_format *plane;
	const struct v4l2_fwht_pixfmt_info *info = &pixfmt_fwht;

	pr_info("dafna: %s: type '%s'\n", __func__,  V4L2_TYPE_IS_OUTPUT(f->type) ? "OUT" : "CAP");
	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		pix = &f->fmt.pix;
		pr_info("%s: before %ux%u ",__func__, pix->height,pix->width);
		if (pix->pixelformat != V4L2_PIX_FMT_FWHT)
			info = find_fmt(pix->pixelformat);

		pix->width = clamp(pix->width, MIN_WIDTH, MAX_WIDTH);
		pix->width = vic_round_dim(pix->width, info->width_div);

		pix->height = clamp(pix->height, MIN_HEIGHT, MAX_HEIGHT);
		pix->height = vic_round_dim(pix->height, info->height_div);

		pix->field = V4L2_FIELD_NONE;
		pix->bytesperline =
			pix->width * info->bytesperline_mult;
		pix->sizeimage = pix->width * pix->height *
			info->sizeimage_mult / info->sizeimage_div;
		pr_info("%s: dafna format from user is '%s'\n",__func__,id_fmt_to_str(pix->pixelformat));
		pr_info("%s: %ux%u d = %ux%u",__func__, pix->height,pix->width,info->height_div,info->width_div);
		pr_info("%s: set bytesperline(%u) = width(%u) * bytesperline_mult (%u)\n",__func__,
				pix->bytesperline,pix->width,info->bytesperline_mult);

		if (pix->pixelformat == V4L2_PIX_FMT_FWHT)
			pix->sizeimage += sizeof(struct fwht_cframe_hdr);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pix_mp = &f->fmt.pix_mp;
		plane = pix_mp->plane_fmt;
		if (pix_mp->pixelformat != V4L2_PIX_FMT_FWHT)
			info = find_fmt(pix_mp->pixelformat);
		pix_mp->num_planes = 1;

		pix_mp->width = clamp(pix_mp->width, MIN_WIDTH, MAX_WIDTH);
		pix_mp->width = vic_round_dim(pix_mp->width, info->width_div);

		pix_mp->height = clamp(pix_mp->height, MIN_HEIGHT, MAX_HEIGHT);
		pix_mp->height = vic_round_dim(pix_mp->height,
					       info->height_div);

		pix_mp->field = V4L2_FIELD_NONE;
		plane->bytesperline =
			pix_mp->width * info->bytesperline_mult;
		plane->sizeimage = pix_mp->width * pix_mp->height *
			info->sizeimage_mult / info->sizeimage_div;
		if (pix_mp->pixelformat == V4L2_PIX_FMT_FWHT)
			plane->sizeimage += sizeof(struct fwht_cframe_hdr);
		memset(pix_mp->reserved, 0, sizeof(pix_mp->reserved));
		memset(plane->reserved, 0, sizeof(plane->reserved));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	struct v4l2_pix_format_mplane *pix_mp;
	struct v4l2_pix_format *pix;
	pr_info("dafna: %s\n",__func__);

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		if (multiplanar)
			return -EINVAL;
		pix = &f->fmt.pix;
		pix->pixelformat = ctx->is_enc ? V4L2_PIX_FMT_FWHT :
				   find_fmt(f->fmt.pix.pixelformat)->id;
		pix->colorspace = ctx->state.colorspace;
		pix->xfer_func = ctx->state.xfer_func;
		pix->ycbcr_enc = ctx->state.ycbcr_enc;
		pix->quantization = ctx->state.quantization;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		if (!multiplanar)
			return -EINVAL;
		pix_mp = &f->fmt.pix_mp;
		pix_mp->pixelformat = ctx->is_enc ? V4L2_PIX_FMT_FWHT :
				      find_fmt(pix_mp->pixelformat)->id;
		pix_mp->colorspace = ctx->state.colorspace;
		pix_mp->xfer_func = ctx->state.xfer_func;
		pix_mp->ycbcr_enc = ctx->state.ycbcr_enc;
		pix_mp->quantization = ctx->state.quantization;
		break;
	default:
		return -EINVAL;
	}

	return vidioc_try_fmt(ctx, f);
}

static int vidioc_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	struct v4l2_pix_format_mplane *pix_mp;
	struct v4l2_pix_format *pix;
	pr_info("dafna: %s\n",__func__);

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		if (multiplanar)
			return -EINVAL;
		pix = &f->fmt.pix;
		pr_info("%s: before %ux%u ",__func__, pix->height,pix->width);
		pix->pixelformat = !ctx->is_enc ? V4L2_PIX_FMT_FWHT :
				   find_fmt(pix->pixelformat)->id;
		if (!pix->colorspace)
			pix->colorspace = V4L2_COLORSPACE_REC709;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		if (!multiplanar)
			return -EINVAL;
		pix_mp = &f->fmt.pix_mp;
		pix_mp->pixelformat = !ctx->is_enc ? V4L2_PIX_FMT_FWHT :
				      find_fmt(pix_mp->pixelformat)->id;
		if (!pix_mp->colorspace)
			pix_mp->colorspace = V4L2_COLORSPACE_REC709;
		break;
	default:
		return -EINVAL;
	}

	return vidioc_try_fmt(ctx, f);
}

static int vidioc_s_fmt(struct vicodec_ctx *ctx, struct v4l2_format *f)
{
	struct vicodec_q_data *q_data;
	struct vb2_queue *vq;
	bool fmt_changed = true;
	struct v4l2_pix_format_mplane *pix_mp;
	struct v4l2_pix_format *pix;

	pr_info("dafna: %s: type '%s'\n", __func__,  V4L2_TYPE_IS_OUTPUT(f->type) ? "OUT" : "CAP");
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		pix = &f->fmt.pix;
		if (ctx->is_enc && V4L2_TYPE_IS_OUTPUT(f->type))
			fmt_changed =
				!q_data->info ||
				q_data->info->id != pix->pixelformat ||
				q_data->coded_width != pix->width ||
				q_data->coded_height != pix->height;

		if (vb2_is_busy(vq) && fmt_changed)
			return -EBUSY;

		pr_info("%s: dafna format is '%s'\n",__func__,id_fmt_to_str(pix->pixelformat));
		if (pix->pixelformat == V4L2_PIX_FMT_FWHT)
			q_data->info = &pixfmt_fwht;
		else
			q_data->info = find_fmt(pix->pixelformat);
		q_data->coded_width = pix->width;
		q_data->coded_height = pix->height;
		q_data->sizeimage = pix->sizeimage;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pix_mp = &f->fmt.pix_mp;
		if (ctx->is_enc && V4L2_TYPE_IS_OUTPUT(f->type))
			fmt_changed =
				!q_data->info ||
				q_data->info->id != pix_mp->pixelformat ||
				q_data->coded_width != pix_mp->width ||
				q_data->coded_height != pix_mp->height;

		if (vb2_is_busy(vq) && fmt_changed)
			return -EBUSY;

		if (pix_mp->pixelformat == V4L2_PIX_FMT_FWHT)
			q_data->info = &pixfmt_fwht;
		else
			q_data->info = find_fmt(pix_mp->pixelformat);
		q_data->coded_width = pix_mp->width;
		q_data->coded_height = pix_mp->height;
		q_data->sizeimage = pix_mp->plane_fmt[0].sizeimage;
		break;
	default:
		return -EINVAL;
	}
	if (q_data->visible_width > q_data->coded_width)
		q_data->visible_width = q_data->coded_width;
	if (q_data->visible_height > q_data->coded_height)
		q_data->visible_height = q_data->coded_height;


	pr_info("%s: Setting format for type %d, coded wxh: %dx%d, visible wxh: %dx%d, fourcc: %08x\n",
			__func__, f->type, q_data->coded_width, q_data->coded_height,
			q_data->visible_width, q_data->visible_height,
			q_data->info->id);
	dprintk(ctx->dev,
		"Setting format for type %d, coded wxh: %dx%d, visible wxh: %dx%d, fourcc: %08x\n",
		f->type, q_data->coded_width, q_data->coded_height,
		q_data->visible_width, q_data->visible_height,
		q_data->info->id);

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	int ret;

	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	return vidioc_s_fmt(file2ctx(file), f);
}

static int vidioc_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	struct v4l2_pix_format_mplane *pix_mp;
	struct v4l2_pix_format *pix;
	int ret;
	pr_info("dafna: in vidioc_s_fmt_vid_out\n");

	pr_info("%s: before %ux%u ",__func__, f->fmt.pix.height,f->fmt.pix.width);
	ret = vidioc_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	ret = vidioc_s_fmt(file2ctx(file), f);
	if (!ret) {
		switch (f->type) {
		case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		case V4L2_BUF_TYPE_VIDEO_OUTPUT:
			pix = &f->fmt.pix;
			ctx->state.colorspace = pix->colorspace;
			ctx->state.xfer_func = pix->xfer_func;
			ctx->state.ycbcr_enc = pix->ycbcr_enc;
			ctx->state.quantization = pix->quantization;
			break;
		case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
			pix_mp = &f->fmt.pix_mp;
			ctx->state.colorspace = pix_mp->colorspace;
			ctx->state.xfer_func = pix_mp->xfer_func;
			ctx->state.ycbcr_enc = pix_mp->ycbcr_enc;
			ctx->state.quantization = pix_mp->quantization;
			break;
		default:
			break;
		}
	}
	return ret;
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	struct vicodec_q_data *q_data;
	enum v4l2_buf_type valid_cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	enum v4l2_buf_type valid_out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (multiplanar) {
		valid_cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		valid_out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	}

	q_data = get_q_data(ctx, s->type);
	if (!q_data)
		return -EINVAL;
	/*
	 * encoder supports only cropping on the OUTPUT buffer
	 * decoder supports only composing on the CAPTURE buffer
	 */
	if ((ctx->is_enc && s->type == valid_out_type) ||
	    (!ctx->is_enc && s->type == valid_cap_type)) {
		switch (s->target) {
		case V4L2_SEL_TGT_COMPOSE:
		case V4L2_SEL_TGT_CROP:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = q_data->visible_width;
			s->r.height = q_data->visible_height;
			return 0;
		case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		case V4L2_SEL_TGT_CROP_DEFAULT:
		case V4L2_SEL_TGT_CROP_BOUNDS:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = q_data->coded_width;
			s->r.height = q_data->coded_height;
			return 0;
		}
	}
	return -EINVAL;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	struct vicodec_q_data *q_data;
	enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (multiplanar)
		out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	q_data = get_q_data(ctx, s->type);
	if (!q_data)
		return -EINVAL;

	if (!ctx->is_enc || s->type != out_type ||
	    s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	s->r.left = 0;
	s->r.top = 0;
	q_data->visible_width = clamp(s->r.width, MIN_WIDTH,
				      q_data->coded_width);
	s->r.width = q_data->visible_width;
	q_data->visible_height = clamp(s->r.height, MIN_HEIGHT,
				       q_data->coded_height);
	s->r.height = q_data->visible_height;
	return 0;
}

static void vicodec_mark_last_buf(struct vicodec_ctx *ctx)
{
	static const struct v4l2_event eos_event = {
		.type = V4L2_EVENT_EOS
	};

	spin_lock(ctx->lock);
	ctx->last_src_buf = v4l2_m2m_last_src_buf(ctx->fh.m2m_ctx);
	pr_info("%s: dafna: called last src buf is %p\n", __func__, ctx->last_src_buf);
	if (!ctx->last_src_buf && ctx->last_dst_buf) {
		pr_info("%s: dafna: setting %p as last\n", __func__, ctx->last_dst_buf);
	
		ctx->last_dst_buf->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &eos_event);
	}
	spin_unlock(ctx->lock);
}

static int vicodec_try_encoder_cmd(struct file *file, void *fh,
				struct v4l2_encoder_cmd *ec)
{
	if (ec->cmd != V4L2_ENC_CMD_STOP)
		return -EINVAL;

	if (ec->flags & V4L2_ENC_CMD_STOP_AT_GOP_END)
		return -EINVAL;

	return 0;
}

static int vicodec_encoder_cmd(struct file *file, void *fh,
			    struct v4l2_encoder_cmd *ec)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	int ret;

	ret = vicodec_try_encoder_cmd(file, fh, ec);
	if (ret < 0)
		return ret;

	vicodec_mark_last_buf(ctx);
	return 0;
}

static int vicodec_try_decoder_cmd(struct file *file, void *fh,
				struct v4l2_decoder_cmd *dc)
{
	if (dc->cmd != V4L2_DEC_CMD_STOP)
		return -EINVAL;

	if (dc->flags & V4L2_DEC_CMD_STOP_TO_BLACK)
		return -EINVAL;

	if (!(dc->flags & V4L2_DEC_CMD_STOP_IMMEDIATELY) && (dc->stop.pts != 0))
		return -EINVAL;

	return 0;
}

static int vicodec_decoder_cmd(struct file *file, void *fh,
			    struct v4l2_decoder_cmd *dc)
{
	struct vicodec_ctx *ctx = file2ctx(file);
	int ret;

	ret = vicodec_try_decoder_cmd(file, fh, dc);
	if (ret < 0)
		return ret;

	pr_info("%s: dafna calling vicodec_mark_last_buf\n", __func__);
	vicodec_mark_last_buf(ctx);
	return 0;
}

static int vicodec_enum_framesizes(struct file *file, void *fh,
				   struct v4l2_frmsizeenum *fsize)
{
	switch (fsize->pixel_format) {
	case V4L2_PIX_FMT_FWHT:
		break;
	default:
		if (find_fmt(fsize->pixel_format)->id == fsize->pixel_format)
			break;
		return -EINVAL;
	}

	if (fsize->index)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;

	fsize->stepwise.min_width = MIN_WIDTH;
	fsize->stepwise.max_width = MAX_WIDTH;
	fsize->stepwise.step_width = 8;
	fsize->stepwise.min_height = MIN_HEIGHT;
	fsize->stepwise.max_height = MAX_HEIGHT;
	fsize->stepwise.step_height = 8;

	return 0;
}

static int vicodec_subscribe_event(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static const struct v4l2_ioctl_ops vicodec_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= vidioc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_cap_mplane = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap_mplane	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap_mplane	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap_mplane	= vidioc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out	= vidioc_s_fmt_vid_out,

	.vidioc_enum_fmt_vid_out_mplane = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out_mplane	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out_mplane	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out_mplane	= vidioc_s_fmt_vid_out,

	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf	= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon	= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff	= v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection	= vidioc_g_selection,
	.vidioc_s_selection	= vidioc_s_selection,

	.vidioc_try_encoder_cmd	= vicodec_try_encoder_cmd,
	.vidioc_encoder_cmd	= vicodec_encoder_cmd,
	.vidioc_try_decoder_cmd	= vicodec_try_decoder_cmd,
	.vidioc_decoder_cmd	= vicodec_decoder_cmd,
	.vidioc_enum_framesizes = vicodec_enum_framesizes,

	.vidioc_subscribe_event = vicodec_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};


/*
 * Queue operations
 */

static int vicodec_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			       unsigned int *nplanes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct vicodec_ctx *ctx = vb2_get_drv_priv(vq);
	struct vicodec_q_data *q_data = get_q_data(ctx, vq->type);
	unsigned int size = q_data->sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int vicodec_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static int vicodec_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vicodec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vicodec_q_data *q_data;

	dprintk(ctx->dev, "type: %d\n", vb->vb2_queue->type);

	q_data = get_q_data(ctx, vb->vb2_queue->type);
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE) {
			dprintk(ctx->dev, "%s field isn't supported\n",
					__func__);
			return -EINVAL;
		}
	}

	if (vb2_plane_size(vb, 0) < q_data->sizeimage) {
		dprintk(ctx->dev,
			"%s data will not fit into plane (%lu < %lu)\n",
			__func__, vb2_plane_size(vb, 0),
			(long)q_data->sizeimage);
		return -EINVAL;
	}

	return 0;
}

static void vicodec_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vicodec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int sz = vb2_get_plane_payload(&vbuf->vb2_buf, 0);
	u8 *p_src = vb2_plane_vaddr(&vbuf->vb2_buf, 0);
	u8 *p = p_src;
	struct vb2_queue *vq_out = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
						   V4L2_BUF_TYPE_VIDEO_OUTPUT);
	struct vb2_queue *vq_cap = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
						   V4L2_BUF_TYPE_VIDEO_CAPTURE);
	bool header_valid = false;
	static const struct v4l2_event rs_event = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	/* buf_queue handles only the first source change event */
	if (ctx->first_source_change_sent) {
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
		return;
	}

	/*
	 * if both queues are streaming, the source change event is
	 * handled in job_ready
	 */
	if (vb2_is_streaming(vq_cap) && vb2_is_streaming(vq_out)) {
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
		return;
	}

	/*
	 * source change event is relevant only for the stateful decoder
	 * in the compressed stream
	 */
	if (ctx->is_stateless || ctx->is_enc ||
	    !V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
		return;
	}

	pr_info("%s start looking for first header\n", __func__);
	do {
		enum vb2_buffer_state state =
			get_next_header(ctx, &p, p_src + sz - p);

		if (ctx->header_size < sizeof(struct fwht_cframe_hdr)) {
			pr_info("%s header_size < sizof(hdr)\n", __func__);
			v4l2_m2m_buf_done(vbuf, state);
			return;
		}
		header_valid = is_header_valid(&ctx->state.header);
		/*
		 * p points right after the end of the header in the
		 * buffer. If the header is invalid we set p to point
		 * to the next byte after the start of the header
		 */
		if (!header_valid) {
			p = p - sizeof(struct fwht_cframe_hdr) + 1;
			pr_info("%s heade invalid\n", __func__);
			if (p < p_src)
				p = p_src;
			ctx->header_size = 0;
			ctx->comp_magic_cnt = 0;
		}

	} while (!header_valid);

	ctx->cur_buf_offset = p - p_src;
	update_capture_data_from_header(ctx);
	ctx->first_source_change_sent = true;
	v4l2_event_queue_fh(&ctx->fh, &rs_event);
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void vicodec_return_bufs(struct vb2_queue *q, u32 state)
{
	struct vicodec_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (vbuf == NULL)
			return;
		spin_lock(ctx->lock);
		v4l2_m2m_buf_done(vbuf, state);
		spin_unlock(ctx->lock);
	}
}

static int vicodec_start_streaming(struct vb2_queue *q,
				   unsigned int count)
{
	struct vicodec_ctx *ctx = vb2_get_drv_priv(q);
	struct vicodec_q_data *q_data = get_q_data(ctx, q->type);
	struct v4l2_fwht_state *state = &ctx->state;
	const struct v4l2_fwht_pixfmt_info *info = q_data->info;
	unsigned int size = q_data->coded_width * q_data->coded_height;
	unsigned int chroma_div;
	unsigned int total_planes_size;
	u8 *new_comp_frame;

	pr_info("dafna: %s buffer type: %s, codec type: %s\n", __func__,
		V4L2_TYPE_IS_OUTPUT(q->type) ? "OUT" : "CAP", ctx->is_enc ? "enc" : "dec");
	pr_info("dafna: %s coded wxh %ux%u \n", __func__, q_data->coded_width, q_data->coded_height);

	if (!info)
		return -EINVAL;

	chroma_div = info->width_div * info->height_div;
	q_data->sequence = 0;

	ctx->last_src_buf = NULL;
	ctx->last_dst_buf = NULL;
	state->gop_cnt = 0;

	if ((V4L2_TYPE_IS_OUTPUT(q->type) && !ctx->is_enc) ||
	    (!V4L2_TYPE_IS_OUTPUT(q->type) && ctx->is_enc))
		return 0;

	if (info->id == V4L2_PIX_FMT_FWHT) {
		vicodec_return_bufs(q, VB2_BUF_STATE_QUEUED);
		return -EINVAL;
	}
	if (info->components_num == 4)
		total_planes_size = 2 * size + 2 * (size / chroma_div);
	else if (info->components_num == 3)
		total_planes_size = size + 2 * (size / chroma_div);
	else
		total_planes_size = size;

	state->visible_width = q_data->visible_width;
	state->visible_height = q_data->visible_height;
	state->coded_width = q_data->coded_width;
	state->coded_height = q_data->coded_height;
	state->stride = q_data->coded_width *
				info->bytesperline_mult;
	pr_info("dafna: state visible = %ux%u, coded = %ux%u\n", state->visible_width, state->visible_height, state->coded_width, state->coded_height);

	state->ref_frame.buf = kvmalloc(total_planes_size, GFP_KERNEL);
	state->ref_frame.luma = state->ref_frame.buf;
	ctx->comp_max_size = total_planes_size;
	new_comp_frame = kvmalloc(ctx->comp_max_size, GFP_KERNEL);

	if (!state->ref_frame.luma || !new_comp_frame) {
		kvfree(state->ref_frame.luma);
		kvfree(new_comp_frame);
		vicodec_return_bufs(q, VB2_BUF_STATE_QUEUED);
		return -ENOMEM;
	}
	/*
	 * if state->compressed_frame was already allocated then
	 * it contain data of the first frame of the new resolution
	 */
	if (state->compressed_frame) {
		if (ctx->comp_size > ctx->comp_max_size)
			ctx->comp_size = ctx->comp_max_size;

		memcpy(new_comp_frame,
		       state->compressed_frame, ctx->comp_size);
	}

	kvfree(state->compressed_frame);
	state->compressed_frame = new_comp_frame;

	if (info->components_num >= 3) {
		state->ref_frame.cb = state->ref_frame.luma + size;
		state->ref_frame.cr = state->ref_frame.cb + size / chroma_div;
	} else {
		state->ref_frame.cb = NULL;
		state->ref_frame.cr = NULL;
	}

	if (info->components_num == 4)
		state->ref_frame.alpha =
			state->ref_frame.cr + size / chroma_div;
	else
		state->ref_frame.alpha = NULL;
	return 0;
}

static void vicodec_stop_streaming(struct vb2_queue *q)
{
	struct vicodec_ctx *ctx = vb2_get_drv_priv(q);

	pr_info("%s: dafna: calling vicodec_return_bufs for q = %p (%s)\n", __func__, q,
			V4L2_TYPE_IS_OUTPUT(q->type) ? "OUT" : "CAP");
	vicodec_return_bufs(q, VB2_BUF_STATE_ERROR);

	if ((!V4L2_TYPE_IS_OUTPUT(q->type) && !ctx->is_enc) ||
	    (V4L2_TYPE_IS_OUTPUT(q->type) && ctx->is_enc)) {
		kvfree(ctx->state.ref_frame.buf);
		ctx->state.ref_frame.buf = NULL;
		ctx->state.ref_frame.luma = NULL;
		ctx->comp_max_size = 0;
		ctx->source_changed = false;
	}
	if (V4L2_TYPE_IS_OUTPUT(q->type) && !ctx->is_enc) {
		ctx->cur_buf_offset = 0;
		ctx->comp_size = 0;
		ctx->header_size = 0;
		ctx->comp_magic_cnt = 0;
		ctx->comp_has_frame = 0;
		ctx->comp_has_next_frame = 0;
	}
}

static void vicodec_buf_request_complete(struct vb2_buffer *vb)
{
	struct vicodec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}


static const struct vb2_ops vicodec_qops = {
	.queue_setup		= vicodec_queue_setup,
	.buf_out_validate	= vicodec_buf_out_validate,
	.buf_prepare		= vicodec_buf_prepare,
	.buf_queue		= vicodec_buf_queue,
	.buf_request_complete	= vicodec_buf_request_complete,
	.start_streaming	= vicodec_start_streaming,
	.stop_streaming		= vicodec_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct vicodec_ctx *ctx = priv;
	int ret;
	pr_info("dafna: %s\n",__func__);

	src_vq->type = (multiplanar ?
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
			V4L2_BUF_TYPE_VIDEO_OUTPUT);
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &vicodec_qops;
	src_vq->mem_ops = &vb2_vmalloc_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	if (ctx->is_enc)
		src_vq->lock = &ctx->dev->stateful_enc.mutex;
	else if (ctx->is_stateless)
		src_vq->lock = &ctx->dev->stateless_dec.mutex;
	else
		src_vq->lock = &ctx->dev->stateful_dec.mutex;
	src_vq->supports_requests = ctx->is_stateless ? true : false;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = (multiplanar ?
			V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
			V4L2_BUF_TYPE_VIDEO_CAPTURE);
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &vicodec_qops;
	dst_vq->mem_ops = &vb2_vmalloc_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = src_vq->lock;

	return vb2_queue_init(dst_vq);
}

#define VICODEC_CID_CUSTOM_BASE		(V4L2_CID_MPEG_BASE | 0xf000)
#define VICODEC_CID_I_FRAME_QP		(VICODEC_CID_CUSTOM_BASE + 0)
#define VICODEC_CID_P_FRAME_QP		(VICODEC_CID_CUSTOM_BASE + 1)
#define VICODEC_CID_STATELESS_FWHT	(VICODEC_CID_CUSTOM_BASE + 2)

static int vicodec_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vicodec_ctx *ctx = container_of(ctrl->handler,
					       struct vicodec_ctx, hdl);
	struct media_request *src_req;
	struct vb2_v4l2_buffer *src_buf;
	struct vb2_buffer *ref_vb2_buf;
	u8 *ref_buf;
	struct v4l2_ctrl_fwht_params* params;
	struct vb2_queue *vq_cap;
	struct fwht_raw_frame *ref_frame;
	int idx;

	pr_info("dafna: %s\n",__func__);

	pr_info("%s: ctrl->handler = %p, name = %s\n", __func__, ctrl->handler, ctrl->name);
	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctx->state.gop_size = ctrl->val;
		return 0;
	case VICODEC_CID_I_FRAME_QP:
		ctx->state.i_frame_qp = ctrl->val;
		return 0;
	case VICODEC_CID_P_FRAME_QP:
		ctx->state.p_frame_qp = ctrl->val;
		return 0;
	case VICODEC_CID_STATELESS_FWHT:
		if (!ctx->cur_req)
			return 0;
		params = (struct v4l2_ctrl_fwht_params*) ctrl->p_new.p;
		vq_cap = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					  V4L2_BUF_TYPE_VIDEO_CAPTURE);
		ref_frame = &ctx->state.ref_frame;

		pr_info("%s: stateless ctrl!\n", __func__);
		src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
		if (!src_buf)
			return 0;
		src_req = src_buf->vb2_buf.req_obj.req;
		if (src_req != ctx->cur_req)
			return 0;

		pr_info("%s: got %ux%u\n", __func__,
			params->width, params->height);

		if(params->width > ctx->state.coded_width ||
		   params->height > ctx->state.coded_height) {
			pr_info("%s: bad w/h max allowed %ux%u\n", __func__,
				ctx->state.coded_width, ctx->state.coded_height);
			ctx->bad_statless_params = true;
			return 0;
		}
		/*
		 * if backward_ref_ts is 0, it means there is no
		 * ref frame, so just return
		 */
		if(params->backward_ref_ts == 0) {
			pr_info("%s: back ts is 0\n", __func__);
			ctx->state.visible_width = params->width;
			ctx->state.visible_height = params->height;
			return 0;
		}
		pr_info("%s: back ts =  %llu\n", __func__, params->backward_ref_ts);
		ref_vb2_buf = vb2_find_timestamp_buf(vq_cap, params->backward_ref_ts, 0);
		/*
		if (idx < 0) {
			pr_info("%s: wrong idx\n", __func__);
			ctx->bad_statless_params = true;
			return 0;
		}
		pr_info("%s: got idx %d\n", __func__, idx);
		ref_v4l2_buf = v4l2_m2m_dst_buf_by_idx(ctx->fh.m2m_ctx, idx);
		*/
		if (!ref_vb2_buf) {
			pr_info("%s: could not get buf from ts\n", __func__);
			ctx->bad_statless_params = true;
			return 0;
		}

		ref_buf = vb2_plane_vaddr(ref_vb2_buf, 0);
		ctx->state.visible_width = params->width;
		ctx->state.visible_height = params->height;
		copy_cap_to_ref(ref_buf, ctx->state.info, &ctx->state);

		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_ctrl_ops vicodec_ctrl_ops = {
	.s_ctrl = vicodec_s_ctrl,
};

static const struct v4l2_ctrl_config vicodec_ctrl_i_frame = {
	.ops = &vicodec_ctrl_ops,
	.id = VICODEC_CID_I_FRAME_QP,
	.name = "FWHT I-Frame QP Value",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 1,
	.max = 31,
	.def = 20,
	.step = 1,
};

static const struct v4l2_ctrl_config vicodec_ctrl_p_frame = {
	.ops = &vicodec_ctrl_ops,
	.id = VICODEC_CID_P_FRAME_QP,
	.name = "FWHT P-Frame QP Value",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 1,
	.max = 31,
	.def = 20,
	.step = 1,
};
bool vicodec_ctrl_equal(const struct v4l2_ctrl *ctrl, u32 idx,
		union v4l2_ctrl_ptr ptr1,
		union v4l2_ctrl_ptr ptr2)
{
	pr_info("%s: got ctrl %p\n", __func__, ctrl);
	pr_info("%s: ctrl->handler = %p\n", __func__, ctrl->handler);
	idx *= ctrl->elem_size;
	return !memcmp(ptr1.p + idx, ptr2.p + idx, ctrl->elem_size);
}



static void vicodec_ctrl_init(const struct v4l2_ctrl *ctrl, u32 idx,
		union v4l2_ctrl_ptr ptr)
{
	pr_info("%s: got ctrl %p\n", __func__, ctrl);
	pr_info("%s: ctrl->handler = %p\n", __func__, ctrl->handler);
	idx *= ctrl->elem_size;
	memset(ptr.p + idx, 0, ctrl->elem_size);
}

void vicodec_ctrl_log(const struct v4l2_ctrl *ctrl)
{
	pr_info("%s: got ctrl %s\n", __func__, ctrl ? ctrl->name : "NULL");
}
int vicodec_ctrl_validate(const struct v4l2_ctrl *ctrl, u32 idx,
		union v4l2_ctrl_ptr ptr)
{
	struct v4l2_ctrl_fwht_params* params = (struct v4l2_ctrl_fwht_params*) ptr.p;
	pr_info("%s: got ctrl %s\n", __func__, ctrl ? ctrl->name : "NULL");
	pr_info("%s: ctrl->handler = %p\n", __func__, ctrl->handler);
	pr_info("%s: ctrl idx %u\n", __func__, idx);
	pr_info("%s: wxh = %ux%u back_ts=%llu\n", __func__, params->width,
		params->height, params->backward_ref_ts);

	return 0;

}
static const struct v4l2_ctrl_type_ops vicodec_type_ops = {
	.log = vicodec_ctrl_log,
	.validate = vicodec_ctrl_validate,
	.init = vicodec_ctrl_init,
	.equal = vicodec_ctrl_equal,
};


static const struct v4l2_ctrl_config vicodec_ctrl_stateless_state = {
	.ops		= &vicodec_ctrl_ops,
	.id		= VICODEC_CID_STATELESS_FWHT,
	.elem_size	= sizeof(struct v4l2_ctrl_fwht_params),
	.name		= "FWHT-Stateless State Params",
	.type		= V4L2_CTRL_TYPE_FWHT_PARAMS,
	.type_ops	= &vicodec_type_ops,
};

/*
 * File operations
 */
static int vicodec_open(struct file *file)
{
	struct video_device *vfd = video_devdata(file);
	struct vicodec_dev *dev = video_drvdata(file);
	struct vicodec_ctx *ctx = NULL;
	struct v4l2_ctrl_handler *hdl;
	unsigned int size;
	int rc = 0;

	pr_info("dafna: %s current: %s %u, vfd = %p\n",__func__, current->comm, current->pid, vfd);
	if (mutex_lock_interruptible(vfd->lock))
		return -ERESTARTSYS;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		pr_info("dafna: %s no mem\n",__func__);
		rc = -ENOMEM;
		goto open_unlock;
	}

	if (vfd == &dev->stateful_enc.vfd)
		ctx->is_enc = true;
	else if (vfd == &dev->stateless_dec.vfd)
		ctx->is_stateless = true;

	pr_info("%s: stateless.vfd = %p is statless: %u\n",__func__, &dev->stateless_dec.vfd, ctx->is_stateless);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;
	hdl = &ctx->hdl;
	pr_info("%s: hdl = %p\n", __func__, hdl);
	v4l2_ctrl_handler_init(hdl, 4);
	v4l2_ctrl_new_std(hdl, &vicodec_ctrl_ops, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
			  1, 16, 1, 10);
	pr_info("%s: calling new_custom\n", __func__);
	v4l2_ctrl_new_custom(hdl, &vicodec_ctrl_i_frame, NULL);
	v4l2_ctrl_new_custom(hdl, &vicodec_ctrl_p_frame, NULL);
	if (ctx->is_stateless)
		v4l2_ctrl_new_custom(hdl, &vicodec_ctrl_stateless_state, NULL);
	if (hdl->error) {
		pr_info("dafna: %s hdl err\n",__func__);
		rc = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		kfree(ctx);
		goto open_unlock;
	}
	ctx->fh.ctrl_handler = hdl;
	v4l2_ctrl_handler_setup(hdl);

	ctx->q_data[V4L2_M2M_SRC].info =
		ctx->is_enc ? v4l2_fwht_get_pixfmt(0) : &pixfmt_fwht;
	ctx->q_data[V4L2_M2M_SRC].coded_width = 1280;
	ctx->q_data[V4L2_M2M_SRC].coded_height = 720;
	ctx->q_data[V4L2_M2M_SRC].visible_width = 1280;
	ctx->q_data[V4L2_M2M_SRC].visible_height = 720;
	size = 1280 * 720 * ctx->q_data[V4L2_M2M_SRC].info->sizeimage_mult /
		ctx->q_data[V4L2_M2M_SRC].info->sizeimage_div;
	if (ctx->is_enc)
		ctx->q_data[V4L2_M2M_SRC].sizeimage = size;
	else
		ctx->q_data[V4L2_M2M_SRC].sizeimage =
			size + sizeof(struct fwht_cframe_hdr);
	if (ctx->is_enc) {
		ctx->q_data[V4L2_M2M_DST] = ctx->q_data[V4L2_M2M_SRC];
		ctx->q_data[V4L2_M2M_DST].info = &pixfmt_fwht;
		ctx->q_data[V4L2_M2M_DST].sizeimage = 1280 * 720 *
			ctx->q_data[V4L2_M2M_DST].info->sizeimage_mult /
			ctx->q_data[V4L2_M2M_DST].info->sizeimage_div +
			sizeof(struct fwht_cframe_hdr);
	} else {
		ctx->q_data[V4L2_M2M_DST].info = NULL;
	}

	ctx->state.colorspace = V4L2_COLORSPACE_REC709;

	if (ctx->is_enc) {
		ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->stateful_enc.m2m_dev,
						    ctx, &queue_init);
		ctx->lock = &dev->stateful_enc.lock;
	} else if (ctx->is_stateless) {
		ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->stateless_dec.m2m_dev,
						    ctx, &queue_init);
		ctx->lock = &dev->stateless_dec.lock;
	} else {
		ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->stateful_dec.m2m_dev,
						    ctx, &queue_init);
		ctx->lock = &dev->stateful_dec.lock;
	}

	if (IS_ERR(ctx->fh.m2m_ctx)) {
		pr_info("dafna: %s ctx m2m err\n",__func__);
		rc = PTR_ERR(ctx->fh.m2m_ctx);

		v4l2_ctrl_handler_free(hdl);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		goto open_unlock;
	}

	v4l2_fh_add(&ctx->fh);

open_unlock:
	mutex_unlock(vfd->lock);
	return rc;
}

static int vicodec_release(struct file *file)
{
	struct video_device *vfd = video_devdata(file);
	struct vicodec_ctx *ctx = file2ctx(file);

	pr_info("%s: dafna: call v4l2_m2m_ctx_release vfd = %p\n", __func__, vfd);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->hdl);
	mutex_lock(vfd->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(vfd->lock);
	kfree(ctx);
	pr_info("%s: dafna: returning 0\n", __func__);

	return 0;
}

static int vicodec_request_validate(struct media_request *req)
{
	struct media_request_object *obj;
	struct v4l2_ctrl_handler *parent_hdl, *hdl;
	struct vicodec_ctx *ctx = NULL;
	struct v4l2_ctrl *ctrl;
	unsigned int count;

	pr_info("%s: start req = %p\n", __func__, req);
	pr_info("%s: start for_each\n", __func__);

	list_for_each_entry(obj, &req->objects, list) {
		struct vb2_buffer *vb;
		pr_info("%s: obj = %p\n", __func__, obj);
		pr_info("%s: is b %d\n", __func__, vb2_request_object_is_buffer(obj));

		if (vb2_request_object_is_buffer(obj)) {
			vb = container_of(obj, struct vb2_buffer, req_obj);
			pr_info("%s: vb = %p\n", __func__, vb);
			ctx = vb2_get_drv_priv(vb->vb2_queue);

			break;
		}
	}

	if (!ctx) {
		pr_err("No buffer was provided with the request\n");
		return -ENOENT;
	}

	count = vb2_request_buffer_cnt(req);
	pr_info("%s: start req = %p count = %u\n", __func__, req, count);
	if (!count) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "No buffer was provided with the request\n");
		return -ENOENT;
	} else if (count > 1) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "More than one buffer was provided with the request\n");
		return -EINVAL;
	}

	pr_info("%s: start for_each\n", __func__);

	parent_hdl = &ctx->hdl;

	hdl = v4l2_ctrl_request_hdl_find(req, parent_hdl);
	pr_info("%s: hdl = %p, parent_hdl = %p\n", __func__, hdl, parent_hdl);
	if (!hdl) {
		v4l2_info(&ctx->dev->v4l2_dev, "Missing codec control(s)\n");
		return -ENOENT;
	}
	ctrl = v4l2_ctrl_request_hdl_ctrl_find(hdl, vicodec_ctrl_stateless_state.id);
	if (!ctrl) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "Missing required codec control\n");
		return -ENOENT;
	}

	if (ctrl->elem_size != vicodec_ctrl_stateless_state.elem_size) {
		v4l2_info(&ctx->dev->v4l2_dev,
			  "wrong size, got %u expected %u\n",
			  ctrl->elem_size,
			  vicodec_ctrl_stateless_state.elem_size);
		return -ENOENT;

	}

	return vb2_request_validate(req);
}

static const struct v4l2_file_operations vicodec_fops = {
	.owner		= THIS_MODULE,
	.open		= vicodec_open,
	.release	= vicodec_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device vicodec_videodev = {
	.name		= VICODEC_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &vicodec_fops,
	.ioctl_ops	= &vicodec_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
};

static const struct media_device_ops vicodec_m2m_media_ops = {
	.req_validate	= vicodec_request_validate,
	.req_queue	= v4l2_m2m_request_queue,
};

static const struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_ready	= job_ready,
};

static int register_instance(struct vicodec_dev *dev,
			     struct vicodec_dev_instance *dev_instance,
			     const char *name, bool is_enc)
{
	struct video_device *vfd;
	int ret;

	spin_lock_init(&dev_instance->lock);
	mutex_init(&dev_instance->mutex);
	dev_instance->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev_instance->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init vicodec enc device\n");
		return PTR_ERR(dev_instance->m2m_dev);
	}

	pr_info("%s: vfd = %p\n", __func__, &dev_instance->vfd);
	dev_instance->vfd = vicodec_videodev;
	vfd = &dev_instance->vfd;
	vfd->lock = &dev_instance->mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	strscpy(vfd->name, name, sizeof(vfd->name));
	vfd->device_caps = V4L2_CAP_STREAMING |
		(multiplanar ? V4L2_CAP_VIDEO_M2M_MPLANE : V4L2_CAP_VIDEO_M2M);
	if (is_enc) {
		v4l2_disable_ioctl(vfd, VIDIOC_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_DECODER_CMD);
	} else {
		v4l2_disable_ioctl(vfd, VIDIOC_ENCODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_ENCODER_CMD);
	}
	video_set_drvdata(vfd, dev);

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device '%s'\n", name);
		v4l2_m2m_release(dev_instance->m2m_dev);
		return ret;
	}
	v4l2_info(&dev->v4l2_dev, "Device '%s' registered as /dev/video%d\n",
		  name, vfd->num);
	return 0;
}

static int vicodec_probe(struct platform_device *pdev)
{
	struct vicodec_dev *dev;
	int ret;

	pr_info("dafna: %s current: %s %u\n",__func__, current->comm, current->pid);
	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

#ifdef CONFIG_MEDIA_CONTROLLER
	dev->mdev.dev = &pdev->dev;
	strscpy(dev->mdev.model, "vicodec", sizeof(dev->mdev.model));
	media_device_init(&dev->mdev);
	dev->mdev.ops = &vicodec_m2m_media_ops;
	dev->v4l2_dev.mdev = &dev->mdev;
#endif

	platform_set_drvdata(pdev, dev);

	if (register_instance(dev, &dev->stateful_enc,
			      "videdev-enc", true))
		goto unreg_dev;

	if (register_instance(dev, &dev->stateful_dec,
			      "videdev-statefull-dec", false))
		goto unreg_sf_enc;

	if (register_instance(dev, &dev->stateless_dec,
			      "videdev-stateless-dec", false))
		goto unreg_sf_dec;

#ifdef CONFIG_MEDIA_CONTROLLER
	ret = v4l2_m2m_register_media_controller(dev->stateful_enc.m2m_dev,
						 &dev->stateful_enc.vfd,
						 MEDIA_ENT_F_PROC_VIDEO_ENCODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem media controller for enc\n");
		goto unreg_m2m;
	}

	ret = v4l2_m2m_register_media_controller(dev->stateful_dec.m2m_dev,
						 &dev->stateful_dec.vfd,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem media controller for dec\n");
		goto unreg_m2m_sf_enc_mc;
	}

	ret = v4l2_m2m_register_media_controller(dev->stateless_dec.m2m_dev,
						 &dev->stateless_dec.vfd,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem media controller for stateless dec\n");
		goto unreg_m2m_sf_dec_mc;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register mem2mem media device\n");
		goto unreg_m2m_sl_dec_mc;
	}
#endif
	pr_info("%s: end ok\n", __func__);
	return 0;

#ifdef CONFIG_MEDIA_CONTROLLER
unreg_m2m_sl_dec_mc:
	v4l2_m2m_unregister_media_controller(dev->stateless_dec.m2m_dev);
unreg_m2m_sf_dec_mc:
	v4l2_m2m_unregister_media_controller(dev->stateful_dec.m2m_dev);
unreg_m2m_sf_enc_mc:
	v4l2_m2m_unregister_media_controller(dev->stateful_enc.m2m_dev);
unreg_m2m:
	video_unregister_device(&dev->stateless_dec.vfd);
	v4l2_m2m_release(dev->stateless_dec.m2m_dev);
#endif
unreg_sf_dec:
	video_unregister_device(&dev->stateful_dec.vfd);
	v4l2_m2m_release(dev->stateful_dec.m2m_dev);
unreg_sf_enc:
	video_unregister_device(&dev->stateful_enc.vfd);
	v4l2_m2m_release(dev->stateful_enc.m2m_dev);
unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);

	pr_info("%s: end not so good ret=%d\n", __func__, ret);
	return ret;
}

static int vicodec_remove(struct platform_device *pdev)
{
	struct vicodec_dev *dev = platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " VICODEC_NAME);

#ifdef CONFIG_MEDIA_CONTROLLER
	media_device_unregister(&dev->mdev);
	v4l2_m2m_unregister_media_controller(dev->stateful_enc.m2m_dev);
	v4l2_m2m_unregister_media_controller(dev->stateful_dec.m2m_dev);
	v4l2_m2m_unregister_media_controller(dev->stateless_dec.m2m_dev);
	media_device_cleanup(&dev->mdev);
#endif

	v4l2_m2m_release(dev->stateful_enc.m2m_dev);
	v4l2_m2m_release(dev->stateful_dec.m2m_dev);
	video_unregister_device(&dev->stateful_enc.vfd);
	video_unregister_device(&dev->stateful_dec.vfd);
	video_unregister_device(&dev->stateless_dec.vfd);
	v4l2_device_unregister(&dev->v4l2_dev);

	return 0;
}

static struct platform_driver vicodec_pdrv = {
	.probe		= vicodec_probe,
	.remove		= vicodec_remove,
	.driver		= {
		.name	= VICODEC_NAME,
	},
};

static void __exit vicodec_exit(void)
{
	platform_driver_unregister(&vicodec_pdrv);
	platform_device_unregister(&vicodec_pdev);
}

static int __init vicodec_init(void)
{
	int ret;

	ret = platform_device_register(&vicodec_pdev);
	if (ret)
		return ret;

	ret = platform_driver_register(&vicodec_pdrv);
	if (ret)
		platform_device_unregister(&vicodec_pdev);

	return ret;
}

module_init(vicodec_init);
module_exit(vicodec_exit);
