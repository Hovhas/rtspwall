/*
 * v4l2.c — V4L2 helpers: opens the decoder, sets up the OUTPUT buffers
 * (compressed H.264 in) and the CAPTURE buffers (decoded NV12 out, exported
 * as dmabuf and bound to DRM framebuffers). See rtspwall.h for the structs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "rtspwall.h"

/* ------------------------------------------------------------ V4L2 helpers */

/* `keep_a`/`keep_b` (or -1) are indices that must NOT be touched: the
 * teardown's timeout/quit escape path in teardown_stream may have to leave
 * a live buffer alone (leak it) instead of destroying it, see the comment
 * there. */
void close_buffers(struct camera *k, int keep_a, int keep_b)
{
	for (int i = 0; i < k->n_out; i++)
		if (k->out[i].map) {
			munmap(k->out[i].map, k->out[i].length);
			k->out[i].map = NULL;
		}
	for (int i = 0; i < k->n_cap; i++) {
		if (i == keep_a || i == keep_b)
			continue;
		if (k->cap[i].fb) {
			/* The fb belongs to the DRM fd and is cleaned up when the
			 * program exits. On reconnect it is removed explicitly. */
			k->cap[i].fb = 0;
		}
		if (k->cap[i].dmafd >= 0) {
			close(k->cap[i].dmafd);
			k->cap[i].dmafd = -1;
		}
	}
	k->n_out = k->n_cap = 0;
	k->n_free = 0;
	k->capture_on = false;
}

int open_decoder(const char *device, struct camera *k, unsigned width, unsigned height)
{
	/* O_NONBLOCK is not optional. The decoder stops consuming OUTPUT until
	 * the CAPTURE queue is set up, and it announces that with an event.
	 * With a blocking DQBUF the thread gets stuck waiting for a buffer that
	 * only becomes free once we read the event — and the event is never
	 * read. */
	k->v4l2fd = open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (k->v4l2fd < 0) {
		log_msg("%s: %s: %s", k->name, device, strerror(errno));
		return -1;
	}

	struct v4l2_format f = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.fmt.pix_mp = {
			.width = width, .height = height,
			.pixelformat = V4L2_PIX_FMT_H264,
			.num_planes = 1,
			.plane_fmt[0].sizeimage = OUTPUT_BUFFER_SIZE,
		},
	};
	if (xioctl(k->v4l2fd, VIDIOC_S_FMT, &f)) {
		log_msg("%s: S_FMT OUTPUT: %s", k->name, strerror(errno));
		return -1;
	}

	struct v4l2_requestbuffers rb = {
		.count = OUTPUT_BUFFERS,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (xioctl(k->v4l2fd, VIDIOC_REQBUFS, &rb)) {
		/* On a Raspberry Pi this is typically the VideoCore firmware's own
		 * heap running out, not Linux CMA: with the default gpu_mem (76 MB)
		 * the fourth concurrent 1080p decoder fails to create its
		 * component ("failed to create component -62 (Not enough GPU
		 * mem?)" in dmesg). gpu_mem=256 in config.txt fixes it. The failure
		 * path also leaves the codec in a bad state — a reboot is needed
		 * to get clean again. */
		log_msg("%s: REQBUFS OUTPUT: %s (out of VideoCore memory? see gpu_mem in config.txt)",
			k->name, strerror(errno));
		return -1;
	}
	if (rb.count < 1 || rb.count > OUTPUT_BUFFERS) {
		/* Never trust the driver's count to fit our fixed arrays. */
		log_msg("%s: REQBUFS OUTPUT: driver granted %u buffers, need 1-%d",
			k->name, rb.count, OUTPUT_BUFFERS);
		return -1;
	}
	k->n_out = rb.count;

	for (int i = 0; i < k->n_out; i++) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i, .length = 1, .m.planes = &pl,
		};
		if (xioctl(k->v4l2fd, VIDIOC_QUERYBUF, &b)) {
			log_msg("%s: QUERYBUF OUTPUT %d: %s", k->name, i, strerror(errno));
			return -1;
		}
		k->out[i].length = pl.length;
		k->out[i].map = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, k->v4l2fd, pl.m.mem_offset);
		if (k->out[i].map == MAP_FAILED) {
			log_msg("%s: mmap OUTPUT %d: %s", k->name, i, strerror(errno));
			k->out[i].map = NULL;
			return -1;
		}
	}

	/* The decoder tells us when it has parsed the SPS and knows the real size. */
	struct v4l2_event_subscription es = { .type = V4L2_EVENT_SOURCE_CHANGE };
	if (xioctl(k->v4l2fd, VIDIOC_SUBSCRIBE_EVENT, &es))
		log_msg("%s: SUBSCRIBE_EVENT: %s (continuing)", k->name, strerror(errno));

	k->n_free = k->n_out;
	for (int i = 0; i < k->n_out; i++)
		k->free_out[i] = i;

	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(k->v4l2fd, VIDIOC_STREAMON, &t)) {
		log_msg("%s: STREAMON OUTPUT: %s", k->name, strerror(errno));
		return -1;
	}
	return 0;
}

/* Set up once the decoder has reported the source format. */
int start_capture(struct wall *v, struct camera *k)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	if (xioctl(k->v4l2fd, VIDIOC_G_FMT, &f)) {
		log_msg("%s: G_FMT CAPTURE: %s", k->name, strerror(errno));
		return -1;
	}

	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl(k->v4l2fd, VIDIOC_S_FMT, &f)) {
		log_msg("%s: S_FMT CAPTURE NV12: %s", k->name, strerror(errno));
		return -1;
	}

	k->fb_width  = f.fmt.pix_mp.width;
	k->fb_height = f.fmt.pix_mp.height;
	uint32_t stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;

	/* The decoder pads the height to a whole macroblock: 1080 becomes 1088.
	 * The buffer is therefore larger than the picture. Without this query
	 * the eight extra lines get scaled into the tile — garbage along the
	 * bottom edge and about one percent of vertical squashing. COMPOSE
	 * gives the visible rectangle. */
	k->vis_x = k->vis_y = 0;
	k->vis_width  = k->fb_width;
	k->vis_height = k->fb_height;

	struct v4l2_selection sel = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.target = V4L2_SEL_TGT_COMPOSE,
	};
	if (xioctl(k->v4l2fd, VIDIOC_G_SELECTION, &sel) == 0
	    && sel.r.width > 0 && sel.r.height > 0) {
		k->vis_x      = sel.r.left;
		k->vis_y      = sel.r.top;
		k->vis_width  = sel.r.width;
		k->vis_height = sel.r.height;
	}

	log_msg("%s: decoder gives %ux%u NV12 (visible %ux%u+%u+%u), stride %u",
		k->name, k->fb_width, k->fb_height,
		k->vis_width, k->vis_height, k->vis_x, k->vis_y, stride);

	struct v4l2_requestbuffers rb = {
		.count = CAPTURE_BUFFERS,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (xioctl(k->v4l2fd, VIDIOC_REQBUFS, &rb)) {
		log_msg("%s: REQBUFS CAPTURE: %s", k->name, strerror(errno));
		return -1;
	}
	if (rb.count < 1 || rb.count > CAPTURE_BUFFERS) {
		log_msg("%s: REQBUFS CAPTURE: driver granted %u buffers, need 1-%d",
			k->name, rb.count, CAPTURE_BUFFERS);
		return -1;
	}
	k->n_cap = rb.count;

	for (int i = 0; i < k->n_cap; i++) {
		struct v4l2_exportbuffer eb = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.index = i, .plane = 0, .flags = O_CLOEXEC | O_RDWR,
		};
		if (xioctl(k->v4l2fd, VIDIOC_EXPBUF, &eb)) {
			log_msg("%s: EXPBUF %d: %s - the decoder cannot export dmabuf",
				k->name, i, strerror(errno));
			return -1;
		}
		k->cap[i].dmafd = eb.fd;

		uint32_t handle = 0;
		if (drmPrimeFDToHandle(v->drmfd, eb.fd, &handle)) {
			log_msg("%s: PrimeFDToHandle %d: %s", k->name, i, strerror(errno));
			return -1;
		}

		/* NV12 contiguous: Y first, then interleaved CbCr. */
		uint32_t h[4] = { handle, handle, 0, 0 };
		uint32_t p[4] = { stride, stride, 0, 0 };
		uint32_t o[4] = { 0, stride * k->fb_height, 0, 0 };

		if (drmModeAddFB2(v->drmfd, k->fb_width, k->fb_height,
				  DRM_FORMAT_NV12, h, p, o, &k->cap[i].fb, 0)) {
			log_msg("%s: AddFB2 %d: %s", k->name, i, strerror(errno));
			return -1;
		}

		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i, .length = 1, .m.planes = &pl,
		};
		if (xioctl(k->v4l2fd, VIDIOC_QBUF, &b)) {
			log_msg("%s: QBUF CAPTURE %d: %s", k->name, i, strerror(errno));
			return -1;
		}
	}

	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(k->v4l2fd, VIDIOC_STREAMON, &t)) {
		log_msg("%s: STREAMON CAPTURE: %s", k->name, strerror(errno));
		return -1;
	}
	k->capture_on = true;
	return 0;
}

/* ----------------------------------------------------------- preflight */

/* True if the M2M device takes H.264 on its OUTPUT (compressed) queue. */
static bool takes_h264(int fd)
{
	for (uint32_t i = 0; i < 64; i++) {
		struct v4l2_fmtdesc d = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
		if (xioctl(fd, VIDIOC_ENUM_FMT, &d))
			return false;
		if (d.pixelformat == V4L2_PIX_FMT_H264)
			return true;
	}
	return false;
}

/* At boot udev may still be applying the "video" group to the device
 * node when the service starts: EACCES/EPERM is retried this long before
 * it counts as a permanent permission problem. */
#define DECODER_ACCESS_WAIT_MS 30000

int decoder_preflight(const char *device)
{
	int fd = -1;
	int e = 0;
	bool told_missing = false, told_access = false;

	/* The codec driver may still be loading at boot: give the device
	 * node up to 15 s to appear before calling it missing, and udev up
	 * to 30 s to set its permissions. */
	for (int waited_ms = 0; !quit; waited_ms += 100) {
		fd = open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
		if (fd >= 0)
			break;
		e = errno;
		if (e == ENOENT && waited_ms < 15000) {
			if (!told_missing)
				log_msg("decoder: %s does not exist yet - waiting up to 15 s", device);
			told_missing = true;
		} else if ((e == EACCES || e == EPERM) && waited_ms < DECODER_ACCESS_WAIT_MS) {
			if (!told_access) {
				log_msg("decoder: cannot open %s: %s - waiting up to %d s (udev may "
					"still be setting its permissions at boot)", device, strerror(e),
					DECODER_ACCESS_WAIT_MS / 1000);
				notify_status("waiting for access to the decoder %s", device);
			}
			told_access = true;
		} else {
			break;
		}
		usleep(100000);
	}
	if (quit && fd < 0)
		return 1;
	if (fd < 0) {
		if (e == ENOENT) {
			log_msg("no H.264 hardware decoder found at %s (Raspberry Pi 5 has none)",
				device);
			log_msg("rtspwall needs the Raspberry Pi 4's (or 3's) H.264 hardware decoder; "
				"if this is a Pi 4, check DECODER= in the config and that "
				"/dev/video10 exists (bcm2835-codec)");
			notify_status("no H.264 hardware decoder at %s (Raspberry Pi 5 has none)",
				      device);
			return RTSPWALL_EXIT_NO_DECODER;
		}
		if (e == EACCES || e == EPERM) {
			log_msg("decoder: cannot open %s: %s - the service user needs access to "
				"the video devices (group \"video\")", device, strerror(e));
			notify_status("no access to the decoder %s: the service user needs the "
				      "group \"video\"", device);
			return RTSPWALL_EXIT_NO_DECODER;
		}
		log_msg("decoder: cannot open %s: %s", device, strerror(e));
		return 1;
	}
	if (told_access)
		log_msg("decoder: %s is accessible now", device);

	struct v4l2_capability cap = { 0 };
	bool ok = false;
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
		/* Not an answer about what the device is: restartable. */
		e = errno;
		close(fd);
		log_msg("decoder: VIDIOC_QUERYCAP on %s failed: %s", device, strerror(e));
		return 1;
	}
	uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
			? cap.device_caps : cap.capabilities;
	ok = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) && takes_h264(fd);
	close(fd);
	if (!ok) {
		log_msg("no H.264 hardware decoder found at %s (Raspberry Pi 5 has none)", device);
		if (cap.driver[0])
			log_msg("decoder: %s is \"%.32s\" (driver %.16s), not a multi-planar H.264 "
				"memory-to-memory decoder - check DECODER= in the config",
				device, (const char *)cap.card, (const char *)cap.driver);
		notify_status("no H.264 hardware decoder at %s (Raspberry Pi 5 has none)", device);
		return RTSPWALL_EXIT_NO_DECODER;
	}
	log_msg("decoder: %s is \"%.32s\" (driver %.16s), H.264 ok", device,
		(const char *)cap.card, (const char *)cap.driver);
	return 0;
}
