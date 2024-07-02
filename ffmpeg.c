#include "ffmpeg.h"
#include <stdio.h>

AVFrame *getFrame(const char *inputfile)
{
    AVFormatContext *format_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVPacket packet;

    // Open the input file
    if (avformat_open_input(&format_ctx, inputfile, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to open input file\n");
        return NULL;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to retrieve stream information\n");
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Find the first video stream
    int video_stream_index = -1;
    for (int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        fprintf(stderr, "No video stream found in the input file\n");
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Allocate codec context
    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Copy codec parameters from stream
    if (avcodec_parameters_to_context(codec_ctx, format_ctx->streams[video_stream_index]->codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to context\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Find the decoder for the codec
    codec = avcodec_find_decoder(codec_ctx->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Open codec
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Allocate frame
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Failed to allocate frame\n");
        avcodec_close(codec_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return NULL;
    }

    // Read frames
    while (av_read_frame(format_ctx, &packet) >= 0) {
        if (packet.stream_index == video_stream_index) {
            int ret = avcodec_send_packet(codec_ctx, &packet);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet to decoder\n");
                av_packet_unref(&packet);
                break;
            }
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_unref(&packet);
                continue;
            } else if (ret < 0) {
                fprintf(stderr, "Error receiving frame from decoder\n");
                av_packet_unref(&packet);
                break;
            }

            av_packet_unref(&packet);
            break; // Only need one frame
        }
        av_packet_unref(&packet);
    }

    // Clean up
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);

    return frame;
}

AVFrame *toARGB(AVFrame *frame)
{

    AVFrame *out_frame = av_frame_alloc();
    if (!out_frame) {
        fprintf(stderr, "Could not allocate destination frame\n");
        return NULL;
    }

    out_frame->height = frame->height;
    out_frame->width = frame->width;
    out_frame->format = AV_PIX_FMT_ARGB;

    if (av_frame_get_buffer(out_frame, 32) < 0) {
        fprintf(stderr, "Could not allocate buffer for destination frame\n");
        av_frame_free(&out_frame);
        return NULL;
    }

    struct SwsContext *sws_ctx = sws_getContext(frame->width, frame->height, frame->format, frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BICUBLIN, NULL, NULL, NULL);
    sws_scale(sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, out_frame->data, out_frame->linesize);
    sws_freeContext(sws_ctx);
    return out_frame;
}