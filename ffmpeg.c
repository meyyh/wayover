#include "ffmpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int video_decoder_open(VideoDecoder *vd, const char *inputfile) {
    if (!vd || !inputfile) return -1;
    memset(vd, 0, sizeof(*vd));
    vd->video_stream_index = -1;

    // Open video file
    if (avformat_open_input(&vd->format_ctx, inputfile, NULL, NULL) < 0) {
        fprintf(stderr, "Error: Failed to open input video '%s'\n", inputfile);
        return -1;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(vd->format_ctx, NULL) < 0) {
        fprintf(stderr, "Error: Failed to retrieve stream information\n");
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    // Locate video stream
    for (unsigned int i = 0; i < vd->format_ctx->nb_streams; i++) {
        if (vd->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vd->video_stream_index = (int)i;
            break;
        }
    }

    if (vd->video_stream_index == -1) {
        fprintf(stderr, "Error: No video stream found in '%s'\n", inputfile);
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    AVCodecParameters *codecpar = vd->format_ctx->streams[vd->video_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Error: Codec '%s' not supported\n", avcodec_get_name(codecpar->codec_id));
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    vd->codec_ctx = avcodec_alloc_context3(codec);
    if (!vd->codec_ctx) {
        fprintf(stderr, "Error: Failed to allocate codec context\n");
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    if (avcodec_parameters_to_context(vd->codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Error: Failed to copy codec parameters to context\n");
        avcodec_free_context(&vd->codec_ctx);
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    // Use 2 threads to keep memory footprint minimal (~120MB) while maintaining fast decode
    vd->codec_ctx->thread_count = 2;

    if (avcodec_open2(vd->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Error: Failed to open decoder\n");
        avcodec_free_context(&vd->codec_ctx);
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    vd->width = vd->codec_ctx->width;
    vd->height = vd->codec_ctx->height;

    // Detect frame rate
    AVRational rate = vd->codec_ctx->framerate;
    if (rate.num > 0 && rate.den > 0) {
        vd->frame_rate = av_q2d(rate);
    } else {
        rate = vd->format_ctx->streams[vd->video_stream_index]->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) {
            vd->frame_rate = av_q2d(rate);
        } else {
            vd->frame_rate = 30.0;
        }
    }
    if (vd->frame_rate <= 0.0 || vd->frame_rate > 240.0) {
        vd->frame_rate = 30.0;
    }

    // Allocate persistent SwsContext for color/pixel conversion
    vd->sws_ctx = sws_getContext(vd->width, vd->height, vd->codec_ctx->pix_fmt,
                                 vd->width, vd->height, AV_PIX_FMT_BGRA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    if (!vd->sws_ctx) {
        fprintf(stderr, "Error: Failed to create swscale context\n");
        avcodec_free_context(&vd->codec_ctx);
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    // Allocate single BGRA buffer for active frame
    vd->bgra_buffer = (uint8_t *)malloc((size_t)vd->width * vd->height * 4);
    if (!vd->bgra_buffer) {
        fprintf(stderr, "Error: Out of memory allocating frame buffer\n");
        sws_freeContext(vd->sws_ctx);
        avcodec_free_context(&vd->codec_ctx);
        avformat_close_input(&vd->format_ctx);
        return -1;
    }

    vd->frame = av_frame_alloc();
    vd->packet = av_packet_alloc();
    if (!vd->frame || !vd->packet) {
        video_decoder_close(vd);
        return -1;
    }

    return 0;
}

int video_decoder_read_next_frame(VideoDecoder *vd, uint8_t *dst_data, int dst_stride) {
    if (!vd || !vd->codec_ctx) return -1;
    int ret;
    int seek_attempts = 0;

    uint8_t *target = dst_data ? dst_data : vd->bgra_buffer;
    int stride = dst_data ? dst_stride : (vd->width * 4);

    while (1) {
        ret = avcodec_receive_frame(vd->codec_ctx, vd->frame);
        if (ret == 0) {
            uint8_t *dst[4] = { target, NULL, NULL, NULL };
            int dst_linesize[4] = { stride, 0, 0, 0 };
            sws_scale(vd->sws_ctx, (const uint8_t * const *)vd->frame->data, vd->frame->linesize,
                      0, vd->height, dst, dst_linesize);
            av_frame_unref(vd->frame);
            return 0;
        }

        ret = av_read_frame(vd->format_ctx, vd->packet);
        if (ret < 0) {
            // End of stream: seek back to 0 and flush decoder to loop video
            if (++seek_attempts > 2) {
                // Prevent infinite loop if file is empty or unseekable
                return -1;
            }
            av_seek_frame(vd->format_ctx, vd->video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(vd->codec_ctx);
            continue;
        }

        if (vd->packet->stream_index == vd->video_stream_index) {
            avcodec_send_packet(vd->codec_ctx, vd->packet);
        }
        av_packet_unref(vd->packet);
    }
}

void video_decoder_close(VideoDecoder *vd) {
    if (!vd) return;
    if (vd->bgra_buffer) {
        free(vd->bgra_buffer);
        vd->bgra_buffer = NULL;
    }
    if (vd->frame) {
        av_frame_free(&vd->frame);
        vd->frame = NULL;
    }
    if (vd->packet) {
        av_packet_free(&vd->packet);
        vd->packet = NULL;
    }
    if (vd->sws_ctx) {
        sws_freeContext(vd->sws_ctx);
        vd->sws_ctx = NULL;
    }
    if (vd->codec_ctx) {
        avcodec_free_context(&vd->codec_ctx);
        vd->codec_ctx = NULL;
    }
    if (vd->format_ctx) {
        avformat_close_input(&vd->format_ctx);
        vd->format_ctx = NULL;
    }
}