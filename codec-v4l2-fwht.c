// SPDX-License-Identifier: LGPL-2.1
/*
 * A V4L2 frontend for the FWHT codec
 *
 * Copyright 2018 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/videodev2.h>
#include "codec-v4l2-fwht.h"

static const struct v4l2_fwht_pixfmt_info v4l2_fwht_pixfmts[] = {
	{ V4L2_PIX_FMT_YUV420,  1, 3, 2, 1, 1, 2, 2, 3, 3, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_YVU420,  1, 3, 2, 1, 1, 2, 2, 3, 3, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_YUV422P, 1, 2, 1, 1, 1, 2, 1, 3, 3, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_NV12,    1, 3, 2, 1, 2, 2, 2, 3, 2, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_NV21,    1, 3, 2, 1, 2, 2, 2, 3, 2, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_NV16,    1, 2, 1, 1, 2, 2, 1, 3, 2, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_NV61,    1, 2, 1, 1, 2, 2, 1, 3, 2, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_NV24,    1, 3, 1, 1, 2, 1, 1, 3, 2, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_NV42,    1, 3, 1, 1, 2, 1, 1, 3, 2, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_YUYV,    2, 2, 1, 2, 4, 2, 1, 3, 1, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_YVYU,    2, 2, 1, 2, 4, 2, 1, 3, 1, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_UYVY,    2, 2, 1, 2, 4, 2, 1, 3, 1, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_VYUY,    2, 2, 1, 2, 4, 2, 1, 3, 1, FWHT_FL_PIXENC_YUV},
	{ V4L2_PIX_FMT_BGR24,   3, 3, 1, 3, 3, 1, 1, 3, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_RGB24,   3, 3, 1, 3, 3, 1, 1, 3, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_HSV24,   3, 3, 1, 3, 3, 1, 1, 3, 1, FWHT_FL_PIXENC_HSV},
	{ V4L2_PIX_FMT_BGR32,   4, 4, 1, 4, 4, 1, 1, 3, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_XBGR32,  4, 4, 1, 4, 4, 1, 1, 3, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_RGB32,   4, 4, 1, 4, 4, 1, 1, 3, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_XRGB32,  4, 4, 1, 4, 4, 1, 1, 3, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_HSV32,   4, 4, 1, 4, 4, 1, 1, 3, 1, FWHT_FL_PIXENC_HSV},
	{ V4L2_PIX_FMT_ARGB32,  4, 4, 1, 4, 4, 1, 1, 4, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_ABGR32,  4, 4, 1, 4, 4, 1, 1, 4, 1, FWHT_FL_PIXENC_RGB},
	{ V4L2_PIX_FMT_GREY,    1, 1, 1, 1, 0, 1, 1, 1, 1, FWHT_FL_PIXENC_RGB},
};

const struct v4l2_fwht_pixfmt_info *v4l2_fwht_default_fmt(u32 width_div,
							  u32 height_div,
							  u32 components_num,
							  u32 pixenc,
							  unsigned int start_idx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(v4l2_fwht_pixfmts); i++) {
		if (v4l2_fwht_pixfmts[i].width_div == width_div &&
		    v4l2_fwht_pixfmts[i].height_div == height_div &&
		    (!pixenc || v4l2_fwht_pixfmts[i].pixenc == pixenc) &&
		    v4l2_fwht_pixfmts[i].components_num == components_num) {
			if (start_idx == 0)
				return v4l2_fwht_pixfmts + i;
			start_idx--;
		}
	}
	return NULL;
}

const struct v4l2_fwht_pixfmt_info *v4l2_fwht_find_pixfmt(u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(v4l2_fwht_pixfmts); i++)
		if (v4l2_fwht_pixfmts[i].id == pixelformat)
			return v4l2_fwht_pixfmts + i;
	return NULL;
}

const struct v4l2_fwht_pixfmt_info *v4l2_fwht_get_pixfmt(u32 idx)
{
	if (idx >= ARRAY_SIZE(v4l2_fwht_pixfmts))
		return NULL;
	return v4l2_fwht_pixfmts + idx;
}

void copy_cap_to_ref(u8 *cap, const struct v4l2_fwht_pixfmt_info *info,
		     struct v4l2_fwht_state *state)
{
	int plane_idx;
	u8 *p_ref = state->ref_frame.buf;

	for (plane_idx = 0; plane_idx < info->planes_num; plane_idx++) {
		int i;
		bool is_chroma_plane = plane_idx == 1 || plane_idx == 2;
		unsigned int h_div = is_chroma_plane ? info->height_div : 1;
		unsigned int w_div = is_chroma_plane ? info->width_div : 1;
		unsigned int step = is_chroma_plane ? info->chroma_step :
			info->luma_alpha_step;
		unsigned int stride_div =
			(info->planes_num == 3 && plane_idx > 0) ? 2 : 1;

		u8 *row_dst = cap;
		u8 *row_ref = p_ref;

		for (i = 0; i < state->visible_height / h_div; i++) {
			memcpy(row_ref, row_dst, step * state->visible_width / w_div);
			row_ref += step * state->coded_width / w_div;
			row_dst += state->stride / stride_div;
		}
		cap += (state->stride / stride_div) * (state->coded_height / h_div);
		p_ref += (step * state->coded_width / w_div) * (state->coded_height / h_div);
	}
}

static int prepare_raw_frame(struct fwht_raw_frame *rf,
			 const struct v4l2_fwht_pixfmt_info *info, u8 *buf,
			 unsigned int size)
{
	rf->luma = buf;
	rf->width_div = info->width_div;
	rf->height_div = info->height_div;
	rf->luma_alpha_step = info->luma_alpha_step;
	rf->chroma_step = info->chroma_step;
	rf->alpha = NULL;
	rf->components_num = info->components_num;

	switch (info->id) {
	case V4L2_PIX_FMT_GREY:
		rf->cb = NULL;
		rf->cr = NULL;
		break;
	case V4L2_PIX_FMT_YUV420:
		rf->cb = rf->luma + size;
		rf->cr = rf->cb + size / 4;
		break;
	case V4L2_PIX_FMT_YVU420:
		rf->cr = rf->luma + size;
		rf->cb = rf->cr + size / 4;
		break;
	case V4L2_PIX_FMT_YUV422P:
		rf->cb = rf->luma + size;
		rf->cr = rf->cb + size / 2;
		break;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV24:
		rf->cb = rf->luma + size;
		rf->cr = rf->cb + 1;
		break;
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_NV42:
		rf->cr = rf->luma + size;
		rf->cb = rf->cr + 1;
		break;
	case V4L2_PIX_FMT_YUYV:
		rf->cb = rf->luma + 1;
		rf->cr = rf->cb + 2;
		break;
	case V4L2_PIX_FMT_YVYU:
		rf->cr = rf->luma + 1;
		rf->cb = rf->cr + 2;
		break;
	case V4L2_PIX_FMT_UYVY:
		rf->cb = rf->luma;
		rf->cr = rf->cb + 2;
		rf->luma++;
		break;
	case V4L2_PIX_FMT_VYUY:
		rf->cr = rf->luma;
		rf->cb = rf->cr + 2;
		rf->luma++;
		break;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_HSV24:
		rf->cr = rf->luma;
		rf->cb = rf->cr + 2;
		rf->luma++;
		break;
	case V4L2_PIX_FMT_BGR24:
		rf->cb = rf->luma;
		rf->cr = rf->cb + 2;
		rf->luma++;
		break;
	case V4L2_PIX_FMT_RGB32:
	case V4L2_PIX_FMT_XRGB32:
	case V4L2_PIX_FMT_HSV32:
		rf->cr = rf->luma + 1;
		rf->cb = rf->cr + 2;
		rf->luma += 2;
		break;
	case V4L2_PIX_FMT_BGR32:
	case V4L2_PIX_FMT_XBGR32:
		rf->cb = rf->luma;
		rf->cr = rf->cb + 2;
		rf->luma++;
		break;
	case V4L2_PIX_FMT_ARGB32:
		rf->alpha = rf->luma;
		rf->cr = rf->luma + 1;
		rf->cb = rf->cr + 2;
		rf->luma += 2;
		break;
	case V4L2_PIX_FMT_ABGR32:
		rf->cb = rf->luma;
		rf->cr = rf->cb + 2;
		rf->luma++;
		rf->alpha = rf->cr + 1;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int v4l2_fwht_encode(struct v4l2_fwht_state *state, u8 *p_in, u8 *p_out)
{
	unsigned int size = state->stride * state->coded_height;
	unsigned int chroma_stride = state->stride;
	const struct v4l2_fwht_pixfmt_info *info = state->info;
	struct fwht_cframe_hdr *p_hdr;
	struct fwht_cframe cf;
	struct fwht_raw_frame rf;
	u32 encoding;
	u32 flags = 0;

	if (!info)
		return -EINVAL;

	if (prepare_raw_frame(&rf, info, p_in, size))
		return -EINVAL;
	if (info->id == V4L2_PIX_FMT_YUV420 ||
	    info->id == V4L2_PIX_FMT_YVU420 ||
	    info->id == V4L2_PIX_FMT_YUV422P)
		chroma_stride /= 2;

	cf.i_frame_qp = state->i_frame_qp;
	cf.p_frame_qp = state->p_frame_qp;
	cf.rlc_data = (__be16 *)(p_out + sizeof(*p_hdr));

	encoding = fwht_encode_frame(&rf, &state->ref_frame, &cf,
				     !state->gop_cnt,
				     state->gop_cnt == state->gop_size - 1,
				     state->visible_width,
				     state->visible_height,
				     state->stride, chroma_stride);
	if (!(encoding & FWHT_FRAME_PCODED))
		state->gop_cnt = 0;
	if (++state->gop_cnt >= state->gop_size)
		state->gop_cnt = 0;

	p_hdr = (struct fwht_cframe_hdr *)p_out;
	p_hdr->magic1 = FWHT_MAGIC1;
	p_hdr->magic2 = FWHT_MAGIC2;
	p_hdr->version = htonl(FWHT_VERSION);
	p_hdr->width = htonl(state->visible_width);
	p_hdr->height = htonl(state->visible_height);
	flags |= (info->components_num - 1) << FWHT_FL_COMPONENTS_NUM_OFFSET;
	flags |= info->pixenc;
	if (encoding & FWHT_LUMA_UNENCODED)
		flags |= FWHT_FL_LUMA_IS_UNCOMPRESSED;
	if (encoding & FWHT_CB_UNENCODED)
		flags |= FWHT_FL_CB_IS_UNCOMPRESSED;
	if (encoding & FWHT_CR_UNENCODED)
		flags |= FWHT_FL_CR_IS_UNCOMPRESSED;
	if (encoding & FWHT_ALPHA_UNENCODED)
		flags |= FWHT_FL_ALPHA_IS_UNCOMPRESSED;
	if (rf.height_div == 1)
		flags |= FWHT_FL_CHROMA_FULL_HEIGHT;
	if (rf.width_div == 1)
		flags |= FWHT_FL_CHROMA_FULL_WIDTH;
	p_hdr->flags = htonl(flags);
	p_hdr->colorspace = htonl(state->colorspace);
	p_hdr->xfer_func = htonl(state->xfer_func);
	p_hdr->ycbcr_enc = htonl(state->ycbcr_enc);
	p_hdr->quantization = htonl(state->quantization);
	p_hdr->size = htonl(cf.size);
	return cf.size + sizeof(*p_hdr);
}

int v4l2_fwht_decode(struct v4l2_fwht_state *state, u8 *p_in, u8 *p_out)
{
	u32 flags;
	struct fwht_cframe cf;
	unsigned int components_num = 3;
	unsigned int version;
	const struct v4l2_fwht_pixfmt_info *info;
	unsigned int hdr_width_div, hdr_height_div;
	struct fwht_raw_frame dst_rf;
	unsigned int dst_chroma_stride = state->stride;
	unsigned int dst_size = state->stride * state->coded_height;
	unsigned int ref_size;

	if (!state->info)
		return -EINVAL;

	info = state->info;

	version = ntohl(state->header.version);
	if (!version || version > FWHT_VERSION) {
		pr_err("version %d is not supported, current version is %d\n",
		       version, FWHT_VERSION);
		return -EINVAL;
	}

	if (state->header.magic1 != FWHT_MAGIC1 ||
	    state->header.magic2 != FWHT_MAGIC2)
		return -EINVAL;

	/* TODO: support resolution changes */
	if (ntohl(state->header.width)  != state->visible_width ||
	    ntohl(state->header.height) != state->visible_height)
		return -EINVAL;

	flags = ntohl(state->header.flags);

	if (version == FWHT_VERSION) {
		if ((flags & FWHT_FL_PIXENC_MSK) != info->pixenc)
			return -EINVAL;
		components_num = 1 + ((flags & FWHT_FL_COMPONENTS_NUM_MSK) >>
				FWHT_FL_COMPONENTS_NUM_OFFSET);
	}

	if (components_num != info->components_num)
		return -EINVAL;

	state->colorspace = ntohl(state->header.colorspace);
	state->xfer_func = ntohl(state->header.xfer_func);
	state->ycbcr_enc = ntohl(state->header.ycbcr_enc);
	state->quantization = ntohl(state->header.quantization);
	cf.rlc_data = (__be16 *)p_in;
	cf.size = ntohl(state->header.size);

	hdr_width_div = (flags & FWHT_FL_CHROMA_FULL_WIDTH) ? 1 : 2;
	hdr_height_div = (flags & FWHT_FL_CHROMA_FULL_HEIGHT) ? 1 : 2;
	if (hdr_width_div != info->width_div ||
	    hdr_height_div != info->height_div)
		return -EINVAL;

	if (prepare_raw_frame(&dst_rf, info, p_out, dst_size))
		return -EINVAL;
	if (info->id == V4L2_PIX_FMT_YUV420 ||
	    info->id == V4L2_PIX_FMT_YVU420 ||
	    info->id == V4L2_PIX_FMT_YUV422P)
		dst_chroma_stride /= 2;

	ref_size = state->coded_width * state->coded_height *
		info->luma_alpha_step;

	if (prepare_raw_frame(&state->ref_frame, info, state->ref_frame.buf,
			      ref_size))
		return -EINVAL;

	if (!fwht_decode_frame(&cf, &state->ref_frame, flags, components_num,
			       state->visible_width, state->visible_height,
			       state->coded_width, &dst_rf, state->stride,
			       dst_chroma_stride))
		return -EINVAL;
	return 0;
}
