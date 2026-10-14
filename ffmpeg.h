#ifndef FFMPEG_H
#define FFMPEG_H

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdbool.h>

typedef struct {
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    struct SwsContext *sws_ctx;
    AVFrame *frame;
    AVPacket *packet;
    uint8_t *bgra_buffer;
    int video_stream_index;
    int width;
    int height;
    double frame_rate;
} VideoDecoder;

int video_decoder_open(VideoDecoder *vd, const char *inputfile);
int video_decoder_read_next_frame(VideoDecoder *vd, uint8_t *dst_data, int dst_stride);
void video_decoder_close(VideoDecoder *vd);

#endif