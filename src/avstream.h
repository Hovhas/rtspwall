/*
 * avstream.h — the one stream choice shared by the wall (camera_thread.c)
 * and `rtspwall probe` (probe.c), so both judge the same stream: the first
 * H.264 video stream, else the first video stream (see
 * layout_pick_video_stream). Only for files that link libavformat.
 */
#ifndef AVSTREAM_H
#define AVSTREAM_H

#include <libavformat/avformat.h>

#include "layout.h"

#define AVSTREAM_MAX 64   /* streams looked at; inputs never come close */

static inline int av_pick_video_stream(const AVFormatContext *fc)
{
	struct layout_stream s[AVSTREAM_MAX];
	int n = fc->nb_streams < AVSTREAM_MAX ? (int)fc->nb_streams : AVSTREAM_MAX;

	for (int i = 0; i < n; i++) {
		const AVCodecParameters *p = fc->streams[i]->codecpar;
		s[i] = (struct layout_stream){
			.video = p->codec_type == AVMEDIA_TYPE_VIDEO,
			.h264 = p->codec_id == AV_CODEC_ID_H264,
		};
	}
	return layout_pick_video_stream(s, n, NULL);
}

#endif
