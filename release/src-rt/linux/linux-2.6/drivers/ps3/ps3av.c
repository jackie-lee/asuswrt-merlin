/*
 * Copyright (C) 2006 Sony Computer Entertainment Inc.
 * Copyright 2006, 2007 Sony Corporation
 *
 * AV backend support for PS3
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/kernel.h>
#include <linux/ioctl.h>

#include <asm/firmware.h>
#include <asm/lv1call.h>
#include <asm/ps3av.h>
#include <asm/ps3.h>

#include "vuart.h"

#define BUFSIZE          4096	/* vuart buf size */
#define PS3AV_BUF_SIZE   512	/* max packet size */

static int timeout = 5000;	/* in msec ( 5 sec ) */
module_param(timeout, int, 0644);

static struct ps3av {
	int available;
	struct mutex mutex;
	struct work_struct work;
	struct completion done;
	struct workqueue_struct *wq;
	int open_count;
	struct ps3_vuart_port_device *dev;

	int region;
	struct ps3av_pkt_av_get_hw_conf av_hw_conf;
	u32 av_port[PS3AV_AV_PORT_MAX + PS3AV_OPT_PORT_MAX];
	u32 opt_port[PS3AV_OPT_PORT_MAX];
	u32 head[PS3AV_HEAD_MAX];
	u32 audio_port;
	int ps3av_mode;
	int ps3av_mode_old;
} ps3av;

static struct ps3_vuart_port_device ps3av_dev = {
	.match_id = PS3_MATCH_ID_AV_SETTINGS
};

/* color space */
#define YUV444 PS3AV_CMD_VIDEO_CS_YUV444_8
#define RGB8   PS3AV_CMD_VIDEO_CS_RGB_8
/* format */
#define XRGB   PS3AV_CMD_VIDEO_FMT_X8R8G8B8
/* aspect */
#define A_N    PS3AV_CMD_AV_ASPECT_4_3
#define A_W    PS3AV_CMD_AV_ASPECT_16_9
static const struct avset_video_mode {
	u32 cs;
	u32 fmt;
	u32 vid;
	u32 aspect;
	u32 x;
	u32 y;
	u32 interlace;
	u32 freq;
} video_mode_table[] = {
	{     0, }, /* auto */
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_480I,       A_N,  720,  480, 1, 60},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_480P,       A_N,  720,  480, 0, 60},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_720P_60HZ,  A_N, 1280,  720, 0, 60},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_1080I_60HZ, A_W, 1920, 1080, 1, 60},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_1080P_60HZ, A_W, 1920, 1080, 0, 60},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_576I,       A_N,  720,  576, 1, 50},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_576P,       A_N,  720,  576, 0, 50},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_720P_50HZ,  A_N, 1280,  720, 0, 50},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_1080I_50HZ, A_W, 1920, 1080, 1, 50},
	{YUV444, XRGB, PS3AV_CMD_VIDEO_VID_1080P_50HZ, A_W, 1920, 1080, 0, 50},
	{  RGB8, XRGB, PS3AV_CMD_VIDEO_VID_WXGA,       A_W, 1280,  768, 0, 60},
	{  RGB8, XRGB, PS3AV_CMD_VIDEO_VID_SXGA,       A_N, 1280, 1024, 0, 60},
	{  RGB8, XRGB, PS3AV_CMD_VIDEO_VID_WUXGA,      A_W, 1920, 1200, 0, 60},
};

/* supported CIDs */
static u32 cmd_table[] = {
	/* init */
	PS3AV_CID_AV_INIT,
	PS3AV_CID_AV_FIN,
	PS3AV_CID_VIDEO_INIT,
	PS3AV_CID_AUDIO_INIT,

	/* set */
	PS3AV_CID_AV_ENABLE_EVENT,
	PS3AV_CID_AV_DISABLE_EVENT,

	PS3AV_CID_AV_VIDEO_CS,
	PS3AV_CID_AV_VIDEO_MUTE,
	PS3AV_CID_AV_VIDEO_DISABLE_SIG,
	PS3AV_CID_AV_AUDIO_PARAM,
	PS3AV_CID_AV_AUDIO_MUTE,
	PS3AV_CID_AV_HDMI_MODE,
	PS3AV_CID_AV_TV_MUTE,

	PS3AV_CID_VIDEO_MODE,
	PS3AV_CID_VIDEO_FORMAT,
	PS3AV_CID_VIDEO_PITCH,

	PS3AV_CID_AUDIO_MODE,
	PS3AV_CID_AUDIO_MUTE,
	PS3AV_CID_AUDIO_ACTIVE,
	PS3AV_CID_AUDIO_INACTIVE,
	PS3AV_CID_AVB_PARAM,

	/* get */
	PS3AV_CID_AV_GET_HW_CONF,
	PS3AV_CID_AV_GET_MONITOR_INFO,

	/* event */
	PS3AV_CID_EVENT_UNPLUGGED,
	PS3AV_CID_EVENT_PLUGGED,
	PS3AV_CID_EVENT_HDCP_DONE,
	PS3AV_CID_EVENT_HDCP_FAIL,
	PS3AV_CID_EVENT_HDCP_AUTH,
	PS3AV_CID_EVENT_HDCP_ERROR,

	0
};

#define PS3AV_EVENT_CMD_MASK           0x10000000
#define PS3AV_EVENT_ID_MASK            0x0000ffff
#define PS3AV_CID_MASK                 0xffffffff
#define PS3AV_REPLY_BIT                0x80000000

#define ps3av_event_get_port_id(cid)   ((cid >> 16) & 0xff)

static u32 *ps3av_search_cmd_table(u32 cid, u32 mask)
{
	u32 *table;
	int i;

	table = cmd_table;
	for (i = 0;; table++, i++) {
		if ((*table & mask) == (cid & mask))
			break;
		if (*table == 0)
			return NULL;
	}
	return table;
}

static int ps3av_parse_event_packet(const struct ps3av_reply_hdr *hdr)
{
	u32 *table;

	if (hdr->cid & PS3AV_EVENT_CMD_MASK) {
		table = ps3av_search_cmd_table(hdr->cid, PS3AV_EVENT_CMD_MASK);
		if (table)
			dev_dbg(&ps3av_dev.core,
				"recv event packet cid:%08x port:0x%x size:%d\n",
				hdr->cid, ps3av_event_get_port_id(hdr->cid),
				hdr->size);
		else
			printk(KERN_ERR
			       "%s: failed event packet, cid:%08x size:%d\n",
			       __func__, hdr->cid, hdr->size);
		return 1;	/* receive event packet */
	}
	return 0;
}

static int ps3av_send_cmd_pkt(const struct ps3av_send_hdr *send_buf,
			      struct ps3av_reply_hdr *recv_buf, int write_len,
			      int read_len)
{
	int res;
	u32 cmd;
	int event;

	if (!ps3av.available)
		return -ENODEV;

	/* send pkt */
	res = ps3av_vuart_write(ps3av.dev, send_buf, write_len);
	if (res < 0) {
		dev_dbg(&ps3av_dev.core,
			"%s: ps3av_vuart_write() failed (result=%d)\n",
			__func__, res);
		return res;
	}

	/* recv pkt */
	cmd = send_buf->cid;
	do {
		/* read header */
		res = ps3av_vuart_read(ps3av.dev, recv_buf, PS3AV_HDR_SIZE,
				       timeout);
		if (res != PS3AV_HDR_SIZE) {
			dev_dbg(&ps3av_dev.core,
				"%s: ps3av_vuart_read() failed (result=%d)\n",
				__func__, res);
			return res;
		}

		/* read body */
		res = ps3av_vuart_read(ps3av.dev, &recv_buf->cid,
				       recv_buf->size, timeout);
		if (res < 0) {
			dev_dbg(&ps3av_dev.core,
				"%s: ps3av_vuart_read() failed (result=%d)\n",
				__func__, res);
			return res;
		}
		res += PS3AV_HDR_SIZE;	/* total len */
		event = ps3av_parse_event_packet(recv_buf);
		/* ret > 0 event packet */
	} while (event);

	if ((cmd | PS3AV_REPLY_BIT) != recv_buf->cid) {
		dev_dbg(&ps3av_dev.core, "%s: reply err (result=%x)\n",
			__func__, recv_buf->cid);
		return -EINVAL;
	}

	return 0;
}

static int ps3av_process_reply_packet(struct ps3av_send_hdr *cmd_buf,
				      const struct ps3av_reply_hdr *recv_buf,
				      int user_buf_size)
{
	int return_len;

	if (recv_buf->version != PS3AV_VERSION) {
		dev_dbg(&ps3av_dev.core, "reply_packet invalid version:%x\n",
			recv_buf->version);
		return -EFAULT;
	}
	return_len = recv_buf->size + PS3AV_HDR_SIZE;
	if (return_len > user_buf_size)
		return_len = user_buf_size;
	memcpy(cmd_buf, recv_buf, return_len);
	return 0;		/* success */
}

void ps3av_set_hdr(u32 cid, u16 size, struct ps3av_send_hdr *hdr)
{
	hdr->version = PS3AV_VERSION;
	hdr->size = size - PS3AV_HDR_SIZE;
	hdr->cid = cid;
}

int ps3av_do_pkt(u32 cid, u16 send_len, size_t usr_buf_size,
		 struct ps3av_send_hdr *buf)
{
	int res = 0;
	static union {
		struct ps3av_reply_hdr reply_hdr;
		u8 raw[PS3AV_BUF_SIZE];
	} recv_buf;

	u32 *table;

	BUG_ON(!ps3av.available);

	mutex_lock(&ps3av.mutex);

	table = ps3av_search_cmd_table(cid, PS3AV_CID_MASK);
	BUG_ON(!table);
	BUG_ON(send_len < PS3AV_HDR_SIZE);
	BUG_ON(usr_buf_size < send_len);
	BUG_ON(usr_buf_size > PS3AV_BUF_SIZE);

	/* create header */
	ps3av_set_hdr(cid, send_len, buf);

	/* send packet via vuart */
	res = ps3av_send_cmd_pkt(buf, &recv_buf.reply_hdr, send_len,
				 usr_buf_size);
	if (res < 0) {
		printk(KERN_ERR
		       "%s: ps3av_send_cmd_pkt() failed (result=%d)\n",
		       __func__, res);
		goto err;
	}

	/* process reply packet */
	res = ps3av_process_reply_packet(buf, &recv_buf.reply_hdr,
					 usr_buf_size);
	if (res < 0) {
		printk(KERN_ERR "%s: put_return_status() failed (result=%d)\n",
		       __func__, res);
		goto err;
	}

	mutex_unlock(&ps3av.mutex);
	return 0;

      err:
	mutex_unlock(&ps3av.mutex);
	printk(KERN_ERR "%s: failed cid:%x res:%d\n", __func__, cid, res);
	return res;
}

static int ps3av_set_av_video_mute(u32 mute)
{
	int i, num_of_av_port, res;

	num_of_av_port = ps3av.av_hw_conf.num_of_hdmi +
			 ps3av.av_hw_conf.num_of_avmulti;
	/* video mute on */
	for (i = 0; i < num_of_av_port; i++) {
		res = ps3av_cmd_av_video_mute(1, &ps3av.av_port[i], mute);
		if (res < 0)
			return -1;
	}

	return 0;
}

static int ps3av_set_video_disable_sig(void)
{
	int i, num_of_hdmi_port, num_of_av_port, res;

	num_of_hdmi_port = ps3av.av_hw_conf.num_of_hdmi;
	num_of_av_port = ps3av.av_hw_conf.num_of_hdmi +
			 ps3av.av_hw_conf.num_of_avmulti;

	/* tv mute */
	for (i = 0; i < num_of_hdmi_port; i++) {
		res = ps3av_cmd_av_tv_mute(ps3av.av_port[i],
					   PS3AV_CMD_MUTE_ON);
		if (res < 0)
			return -1;
	}
	msleep(100);

	/* video mute on */
	for (i = 0; i < num_of_av_port; i++) {
		res = ps3av_cmd_av_video_disable_sig(ps3av.av_port[i]);
		if (res < 0)
			return -1;
		if (i < num_of_hdmi_port) {
			res = ps3av_cmd_av_tv_mute(ps3av.av_port[i],
						   PS3AV_CMD_MUTE_OFF);
			if (res < 0)
				return -1;
		}
	}
	msleep(300);

	return 0;
}

static int ps3av_set_audio_mute(u32 mute)
{
	int i, num_of_av_port, num_of_opt_port, res;

	num_of_av_port = ps3av.av_hw_conf.num_of_hdmi +
			 ps3av.av_hw_conf.num_of_avmulti;
	num_of_opt_port = ps3av.av_hw_conf.num_of_spdif;

	for (i = 0; i < num_of_av_port; i++) {
		res = ps3av_cmd_av_audio_mute(1, &ps3av.av_port[i], mute);
		if (res < 0)
			return -1;
	}
	for (i = 0; i < num_of_opt_port; i++) {
		res = ps3av_cmd_audio_mute(1, &ps3av.opt_port[i], mute);
		if (res < 0)
			return -1;
	}

	return 0;
}

int ps3av_set_audio_mode(u32 ch, u32 fs, u32 word_bits, u32 format, u32 source)
{
	struct ps3av_pkt_avb_param avb_param;
	int i, num_of_audio, vid, res;
	struct ps3av_pkt_audio_mode audio_mode;
	u32 len = 0;

	num_of_audio = ps3av.av_hw_conf.num_of_hdmi +
		       ps3av.av_hw_conf.num_of_avmulti +
		       ps3av.av_hw_conf.num_of_spdif;

	avb_param.num_of_video_pkt = 0;
	avb_param.num_of_audio_pkt = PS3AV_AVB_NUM_AUDIO;	/* always 0 */
	avb_param.num_of_av_video_pkt = 0;
	avb_param.num_of_av_audio_pkt = ps3av.av_hw_conf.num_of_hdmi;

	vid = video_mode_table[ps3av.ps3av_mode].vid;

	/* audio mute */
	ps3av_set_audio_mute(PS3AV_CMD_MUTE_ON);

	/* audio inactive */
	res = ps3av_cmd_audio_active(0, ps3av.audio_port);
	if (res < 0)
		dev_dbg(&ps3av_dev.core,
			"ps3av_cmd_audio_active OFF failed\n");

	/* audio_pkt */
	for (i = 0; i < num_of_audio; i++) {
		ps3av_cmd_set_audio_mode(&audio_mode, ps3av.av_port[i], ch, fs,
					 word_bits, format, source);
		if (i < ps3av.av_hw_conf.num_of_hdmi) {
			/* hdmi only */
			len += ps3av_cmd_set_av_audio_param(&avb_param.buf[len],
							    ps3av.av_port[i],
							    &audio_mode, vid);
		}
		/* audio_mode pkt should be sent separately */
		res = ps3av_cmd_audio_mode(&audio_mode);
		if (res < 0)
			dev_dbg(&ps3av_dev.core,
				"ps3av_cmd_audio_mode failed, port:%x\n", i);
	}

	/* send command using avb pkt */
	len += offsetof(struct ps3av_pkt_avb_param, buf);
	res = ps3av_cmd_avb_param(&avb_param, len);
	if (res < 0)
		dev_dbg(&ps3av_dev.core, "ps3av_cmd_avb_param failed\n");

	/* audio mute */
	ps3av_set_audio_mute(PS3AV_CMD_MUTE_OFF);

	/* audio active */
	res = ps3av_cmd_audio_active(1, ps3av.audio_port);
	if (res < 0)
		dev_dbg(&ps3av_dev.core, "ps3av_cmd_audio_active ON failed\n");

	return 0;
}

EXPORT_SYMBOL_GPL(ps3av_set_audio_mode);

static int ps3av_set_videomode(void)
{
	/* av video mute */
	ps3av_set_av_video_mute(PS3AV_CMD_MUTE_ON);

	/* wake up ps3avd to do the actual video mode setting */
	queue_work(ps3av.wq, &ps3av.work);

	return 0;
}

static void ps3av_set_videomode_cont(u32 id, u32 old_id)
{
	struct ps3av_pkt_avb_param avb_param;
	int i;
	u32 len = 0, av_video_cs;
	const struct avset_video_mode *video_mode;
	int res;

	video_mode = &video_mode_table[id & PS3AV_MODE_MASK];

	avb_param.num_of_video_pkt = PS3AV_AVB_NUM_VIDEO;	/* num of head */
	avb_param.num_of_audio_pkt = 0;
	avb_param.num_of_av_video_pkt = ps3av.av_hw_conf.num_of_hdmi +
					ps3av.av_hw_conf.num_of_avmulti;
	avb_param.num_of_av_audio_pkt = 0;

	/* video signal off */
	ps3av_set_video_disable_sig();

	/* Retail PS3 product doesn't support this */
	if (id & PS3AV_MODE_HDCP_OFF) {
		res = ps3av_cmd_av_hdmi_mode(PS3AV_CMD_AV_HDMI_HDCP_OFF);
		if (res == PS3AV_STATUS_UNSUPPORTED_HDMI_MODE)
			dev_dbg(&ps3av_dev.core, "Not supported\n");
		else if (res)
			dev_dbg(&ps3av_dev.core,
				"ps3av_cmd_av_hdmi_mode failed\n");
	} else if (old_id & PS3AV_MODE_HDCP_OFF) {
		res = ps3av_cmd_av_hdmi_mode(PS3AV_CMD_AV_HDMI_MODE_NORMAL);
		if (res < 0 && res != PS3AV_STATUS_UNSUPPORTED_HDMI_MODE)
			dev_dbg(&ps3av_dev.core,
				"ps3av_cmd_av_hdmi_mode failed\n");
	}

	/* video_pkt */
	for (i = 0; i < avb_param.num_of_video_pkt; i++)
		len += ps3av_cmd_set_video_mode(&avb_param.buf[len],
						ps3av.head[i], video_mode->vid,
						video_mode->fmt, id);
	/* av_video_pkt */
	for (i = 0; i < avb_param.num_of_av_video_pkt; i++) {
		if (id & PS3AV_MODE_DVI || id & PS3AV_MODE_RGB)
			av_video_cs = RGB8;
		else
			av_video_cs = video_mode->cs;
#ifndef PS3AV_HDMI_YUV
		if (ps3av.av_port[i] == PS3AV_CMD_AVPORT_HDMI_0 ||
		    ps3av.av_port[i] == PS3AV_CMD_AVPORT_HDMI_1)
			av_video_cs = RGB8;	/* use RGB for HDMI */
#endif
		len += ps3av_cmd_set_av_video_cs(&avb_param.buf[len],
						 ps3av.av_port[i],
						 video_mode->vid, av_video_cs,
						 video_mode->aspect, id);
	}
	/* send command using avb pkt */
	len += offsetof(struct ps3av_pkt_avb_param, buf);
	res = ps3av_cmd_avb_param(&avb_param, len);
	if (res == PS3AV_STATUS_NO_SYNC_HEAD)
		printk(KERN_WARNING
		       "%s: Command failed. Please try your request again. \n",
		       __func__);
	else if (res)
		dev_dbg(&ps3av_dev.core, "ps3av_cmd_avb_param failed\n");

	msleep(1500);
	/* av video mute */
	ps3av_set_av_video_mute(PS3AV_CMD_MUTE_OFF);
}

static void ps3avd(struct work_struct *work)
{
	ps3av_set_videomode_cont(ps3av.ps3av_mode, ps3av.ps3av_mode_old);
	complete(&ps3av.done);
}

static int ps3av_vid2table_id(int vid)
{
	int i;

	for (i = 1; i < ARRAY_SIZE(video_mode_table); i++)
		if (video_mode_table[i].vid == vid)
			return i;
	return -1;
}

static int ps3av_resbit2vid(u32 res_50, u32 res_60)
{
	int vid = -1;

	if (res_50 > res_60) {	/* if res_50 == res_60, res_60 will be used */
		if (res_50 & PS3AV_RESBIT_1920x1080P)
			vid = PS3AV_CMD_VIDEO_VID_1080P_50HZ;
		else if (res_50 & PS3AV_RESBIT_1920x1080I)
			vid = PS3AV_CMD_VIDEO_VID_1080I_50HZ;
		else if (res_50 & PS3AV_RESBIT_1280x720P)
			vid = PS3AV_CMD_VIDEO_VID_720P_50HZ;
		else if (res_50 & PS3AV_RESBIT_720x576P)
			vid = PS3AV_CMD_VIDEO_VID_576P;
		else
			vid = -1;
	} else {
		if (res_60 & PS3AV_RESBIT_1920x1080P)
			vid = PS3AV_CMD_VIDEO_VID_1080P_60HZ;
		else if (res_60 & PS3AV_RESBIT_1920x1080I)
			vid = PS3AV_CMD_VIDEO_VID_1080I_60HZ;
		else if (res_60 & PS3AV_RESBIT_1280x720P)
			vid = PS3AV_CMD_VIDEO_VID_720P_60HZ;
		else if (res_60 & PS3AV_RESBIT_720x480P)
			vid = PS3AV_CMD_VIDEO_VID_480P;
		else
			vid = -1;
	}
	return vid;
}

static int ps3av_hdmi_get_vid(struct ps3av_info_monitor *info)
{
	u32 res_50, res_60;
	int vid = -1;

	if (info->monitor_type != PS3AV_MONITOR_TYPE_HDMI)
		return -1;

	/* check native resolution */
	res_50 = info->res_50.native & PS3AV_RES_MASK_50;
	res_60 = info->res_60.native & PS3AV_RES_MASK_60;
	if (res_50 || res_60) {
		vid = ps3av_resbit2vid(res_50, res_60);
		return vid;
	}

	/* check resolution */
	res_50 = info->res_50.res_bits & PS3AV_RES_MASK_50;
	res_60 = info->res_60.res_bits & PS3AV_RES_MASK_60;
	if (res_50 || res_60) {
		vid = ps3av_resbit2vid(res_50, res_60);
		return vid;
	}

	if (ps3av.region & PS3AV_REGION_60)
		vid = PS3AV_DEFAULT_HDMI_VID_REG_60;
	else
		vid = PS3AV_DEFAULT_HDMI_VID_REG_50;
	return vid;
}

static int ps3av_auto_videomode(struct ps3av_pkt_av_get_hw_conf *av_hw_conf,
				int boot)
{
	int i, res, vid = -1, dvi = 0, rgb = 0;
	struct ps3av_pkt_av_get_monitor_info monitor_info;
	struct ps3av_info_monitor *info;

	/* get vid for hdmi */
	for (i = 0; i < av_hw_conf->num_of_hdmi; i++) {
		res = ps3av_cmd_video_get_monitor_info(&monitor_info,
						       PS3AV_CMD_AVPORT_HDMI_0 +
						       i);
		if (res < 0)
			return -1;

		ps3av_cmd_av_monitor_info_dump(&monitor_info);
		info = &monitor_info.info;
		/* check DVI */
		if (info->monitor_type == PS3AV_MONITOR_TYPE_DVI) {
			dvi = PS3AV_MODE_DVI;
			break;
		}
		/* check HDMI */
		vid = ps3av_hdmi_get_vid(info);
		if (vid != -1) {
			/* got valid vid */
			break;
		}
	}

	if (dvi) {
		/* DVI mode */
		vid = PS3AV_DEFAULT_DVI_VID;
	} else if (vid == -1) {
		/* no HDMI interface or HDMI is off */
		if (ps3av.region & PS3AV_REGION_60)
			vid = PS3AV_DEFAULT_AVMULTI_VID_REG_60;
		else
			vid = PS3AV_DEFAULT_AVMULTI_VID_REG_50;
		if (ps3av.region & PS3AV_REGION_RGB)
			rgb = PS3AV_MODE_RGB;
	} else if (boot) {
		/* HDMI: using DEFAULT HDMI_VID while booting up */
		info = &monitor_info.info;
		if (ps3av.region & PS3AV_REGION_60) {
			if (info->res_60.res_bits & PS3AV_RESBIT_720x480P)
				vid = PS3AV_DEFAULT_HDMI_VID_REG_60;
			else if (info->res_50.res_bits & PS3AV_RESBIT_720x576P)
				vid = PS3AV_DEFAULT_HDMI_VID_REG_50;
			else {
				/* default */
				vid = PS3AV_DEFAULT_HDMI_VID_REG_60;
			}
		} else {
			if (info->res_50.res_bits & PS3AV_RESBIT_720x576P)
				vid = PS3AV_DEFAULT_HDMI_VID_REG_50;
			else if (info->res_60.res_bits & PS3AV_RESBIT_720x480P)
				vid = PS3AV_DEFAULT_HDMI_VID_REG_60;
			else {
				/* default */
				vid = PS3AV_DEFAULT_HDMI_VID_REG_50;
			}
		}
	}

	return (ps3av_vid2table_id(vid) | dvi | rgb);
}

static int ps3av_get_hw_conf(struct ps3av *ps3av)
{
	int i, j, k, res;

	/* get av_hw_conf */
	res = ps3av_cmd_av_get_hw_conf(&ps3av->av_hw_conf);
	if (res < 0)
		return -1;

	ps3av_cmd_av_hw_conf_dump(&ps3av->av_hw_conf);

	for (i = 0; i < PS3AV_HEAD_MAX; i++)
		ps3av->head[i] = PS3AV_CMD_VIDEO_HEAD_A + i;
	for (i = 0; i < PS3AV_OPT_PORT_MAX; i++)
		ps3av->opt_port[i] = PS3AV_CMD_AVPORT_SPDIF_0 + i;
	for (i = 0; i < ps3av->av_hw_conf.num_of_hdmi; i++)
		ps3av->av_port[i] = PS3AV_CMD_AVPORT_HDMI_0 + i;
	for (j = 0; j < ps3av->av_hw_conf.num_of_avmulti; j++)
		ps3av->av_port[i + j] = PS3AV_CMD_AVPORT_AVMULTI_0 + j;
	for (k = 0; k < ps3av->av_hw_conf.num_of_spdif; k++)
		ps3av->av_port[i + j + k] = PS3AV_CMD_AVPORT_SPDIF_0 + k;

	/* set all audio port */
	ps3av->audio_port = PS3AV_CMD_AUDIO_PORT_HDMI_0
	    | PS3AV_CMD_AUDIO_PORT_HDMI_1
	    | PS3AV_CMD_AUDIO_PORT_AVMULTI_0
	    | PS3AV_CMD_AUDIO_PORT_SPDIF_0 | PS3AV_CMD_AUDIO_PORT_SPDIF_1;

	return 0;
}

/* set mode using id */
int ps3av_set_video_mode(u32 id, int boot)
{
	int size;
	u32 option;

	size = ARRAY_SIZE(video_mode_table);
	if ((id & PS3AV_MODE_MASK) > size - 1 || id < 0) {
		dev_dbg(&ps3av_dev.core, "%s: error id :%d\n", __func__, id);
		return -EINVAL;
	}

	/* auto mode */
	option = id & ~PS3AV_MODE_MASK;
	if ((id & PS3AV_MODE_MASK) == 0) {
		id = ps3av_auto_videomode(&ps3av.av_hw_conf, boot);
		if (id < 1) {
			printk(KERN_ERR "%s: invalid id :%d\n", __func__, id);
			return -EINVAL;
		}
		id |= option;
	}

	/* set videomode */
	wait_for_completion(&ps3av.done);
	ps3av.ps3av_mode_old = ps3av.ps3av_mode;
	ps3av.ps3av_mode = id;
	if (ps3av_set_videomode())
		ps3av.ps3av_mode = ps3av.ps3av_mode_old;

	return 0;
}

EXPORT_SYMBOL_GPL(ps3av_set_video_mode);

int ps3av_get_auto_mode(int boot)
{
	return ps3av_auto_videomode(&ps3av.av_hw_conf, boot);
}

EXPORT_SYMBOL_GPL(ps3av_get_auto_mode);

int ps3av_set_mode(u32 id, int boot)
{
	int res;

	res = ps3av_set_video_mode(id, boot);
	if (res)
		return res;

	res = ps3av_set_audio_mode(PS3AV_CMD_AUDIO_NUM_OF_CH_2,
				   PS3AV_CMD_AUDIO_FS_48K,
				   PS3AV_CMD_AUDIO_WORD_BITS_16,
				   PS3AV_CMD_AUDIO_FORMAT_PCM,
				   PS3AV_CMD_AUDIO_SOURCE_SERIAL);
	if (res)
		return res;

	return 0;
}

EXPORT_SYMBOL_GPL(ps3av_set_mode);

int ps3av_get_mode(void)
{
	return ps3av.ps3av_mode;
}

EXPORT_SYMBOL_GPL(ps3av_get_mode);

int ps3av_get_scanmode(int id)
{
	int size;

	id = id & PS3AV_MODE_MASK;
	size = ARRAY_SIZE(video_mode_table);
	if (id > size - 1 || id < 0) {
		printk(KERN_ERR "%s: invalid mode %d\n", __func__, id);
		return -EINVAL;
	}
	return video_mode_table[id].interlace;
}

EXPORT_SYMBOL_GPL(ps3av_get_scanmode);

int ps3av_get_refresh_rate(int id)
{
	int size;

	id = id & PS3AV_MODE_MASK;
	size = ARRAY_SIZE(video_mode_table);
	if (id > size - 1 || id < 0) {
		printk(KERN_ERR "%s: invalid mode %d\n", __func__, id);
		return -EINVAL;
	}
	return video_mode_table[id].freq;
}

EXPORT_SYMBOL_GPL(ps3av_get_refresh_rate);

/* get resolution by video_mode */
int ps3av_video_mode2res(u32 id, u32 *xres, u32 *yres)
{
	int size;

	id = id & PS3AV_MODE_MASK;
	size = ARRAY_SIZE(video_mode_table);
	if (id > size - 1 || id < 0) {
		printk(KERN_ERR "%s: invalid mode %d\n", __func__, id);
		return -EINVAL;
	}
	*xres = video_mode_table[id].x;
	*yres = video_mode_table[id].y;
	return 0;
}

EXPORT_SYMBOL_GPL(ps3av_video_mode2res);

/* mute */
int ps3av_video_mute(int mute)
{
	return ps3av_set_av_video_mute(mute ? PS3AV_CMD_MUTE_ON
					    : PS3AV_CMD_MUTE_OFF);
}

EXPORT_SYMBOL_GPL(ps3av_video_mute);

int ps3av_audio_mute(int mute)
{
	return ps3av_set_audio_mute(mute ? PS3AV_CMD_MUTE_ON
					 : PS3AV_CMD_MUTE_OFF);
}

EXPORT_SYMBOL_GPL(ps3av_audio_mute);

int ps3av_dev_open(void)
{
	int status = 0;

	mutex_lock(&ps3av.mutex);
	if (!ps3av.open_count++) {
		status = lv1_gpu_open(0);
		if (status) {
			printk(KERN_ERR "%s: lv1_gpu_open failed %d\n",
			       __func__, status);
			ps3av.open_count--;
		}
	}
	mutex_unlock(&ps3av.mutex);

	return status;
}

EXPORT_SYMBOL_GPL(ps3av_dev_open);

int ps3av_dev_close(void)
{
	int status = 0;

	mutex_lock(&ps3av.mutex);
	if (ps3av.open_count <= 0) {
		printk(KERN_ERR "%s: GPU already closed\n", __func__);
		status = -1;
	} else if (!--ps3av.open_count) {
		status = lv1_gpu_close();
		if (status)
			printk(KERN_WARNING "%s: lv1_gpu_close failed %d\n",
			       __func__, status);
	}
	mutex_unlock(&ps3av.mutex);

	return status;
}

EXPORT_SYMBOL_GPL(ps3av_dev_close);

static int ps3av_probe(struct ps3_vuart_port_device *dev)
{
	int res;
	u32 id;

	dev_dbg(&ps3av_dev.core, "init ...\n");
	dev_dbg(&ps3av_dev.core, "  timeout=%d\n", timeout);

	memset(&ps3av, 0, sizeof(ps3av));

	mutex_init(&ps3av.mutex);
	ps3av.ps3av_mode = 0;
	ps3av.dev = dev;

	INIT_WORK(&ps3av.work, ps3avd);
	init_completion(&ps3av.done);
	complete(&ps3av.done);
	ps3av.wq = create_singlethread_workqueue("ps3avd");
	if (!ps3av.wq)
		return -ENOMEM;

	ps3av.available = 1;
	switch (ps3_os_area_get_av_multi_out()) {
	case PS3_PARAM_AV_MULTI_OUT_NTSC:
		ps3av.region = PS3AV_REGION_60;
		break;
	case PS3_PARAM_AV_MULTI_OUT_PAL_YCBCR:
	case PS3_PARAM_AV_MULTI_OUT_SECAM:
		ps3av.region = PS3AV_REGION_50;
		break;
	case PS3_PARAM_AV_MULTI_OUT_PAL_RGB:
		ps3av.region = PS3AV_REGION_50 | PS3AV_REGION_RGB;
		break;
	default:
		ps3av.region = PS3AV_REGION_60;
		break;
	}

	/* init avsetting modules */
	res = ps3av_cmd_init();
	if (res < 0)
		printk(KERN_ERR "%s: ps3av_cmd_init failed %d\n", __func__,
		       res);

	ps3av_get_hw_conf(&ps3av);
	id = ps3av_auto_videomode(&ps3av.av_hw_conf, 1);
	mutex_lock(&ps3av.mutex);
	ps3av.ps3av_mode = id;
	mutex_unlock(&ps3av.mutex);

	dev_dbg(&ps3av_dev.core, "init...done\n");

	return 0;
}

static int ps3av_remove(struct ps3_vuart_port_device *dev)
{
	if (ps3av.available) {
		ps3av_cmd_fin();
		if (ps3av.wq)
			destroy_workqueue(ps3av.wq);
		ps3av.available = 0;
	}

	return 0;
}

static void ps3av_shutdown(struct ps3_vuart_port_device *dev)
{
	ps3av_remove(dev);
}

static struct ps3_vuart_port_driver ps3av_driver = {
	.match_id = PS3_MATCH_ID_AV_SETTINGS,
	.core = {
		.name = "ps3_av",
	},
	.probe = ps3av_probe,
	.remove = ps3av_remove,
	.shutdown = ps3av_shutdown,
};

static int ps3av_module_init(void)
{
	int error;

	if (!firmware_has_feature(FW_FEATURE_PS3_LV1))
		return -ENODEV;

	error = ps3_vuart_port_driver_register(&ps3av_driver);
	if (error) {
		printk(KERN_ERR
		       "%s: ps3_vuart_port_driver_register failed %d\n",
		       __func__, error);
		return error;
	}

	error = ps3_vuart_port_device_register(&ps3av_dev);
	if (error)
		printk(KERN_ERR
		       "%s: ps3_vuart_port_device_register failed %d\n",
		       __func__, error);

	return error;
}

static void __exit ps3av_module_exit(void)
{
	device_unregister(&ps3av_dev.core);
	ps3_vuart_port_driver_unregister(&ps3av_driver);
}

subsys_initcall(ps3av_module_init);
module_exit(ps3av_module_exit);
