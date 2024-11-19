#include "ffmpeg.h"
#include <stdio.h>

FrameArray getFrames(const char *inputfile) {
    AVFormatContext *format_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVPacket packet;
    FrameArray frame_array = {NULL, 0};
    int ret;

    // Open the input file
    if (avformat_open_input(&format_ctx, inputfile, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to open input file\n");
        return frame_array;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to retrieve stream information\n");
        avformat_close_input(&format_ctx);
        return frame_array;
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
        return frame_array;
    }

    // Allocate codec context
    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        avformat_close_input(&format_ctx);
        return frame_array;
    }

    // Copy codec parameters from stream
    if (avcodec_parameters_to_context(codec_ctx, format_ctx->streams[video_stream_index]->codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to context\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return frame_array;
    }

    // Find the decoder for the codec
    codec = avcodec_find_decoder(codec_ctx->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return frame_array;
    }

    // Open codec
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return frame_array;
    }

    frame_array.frame_rate = av_q2d(codec_ctx->framerate);
    if (frame_array.frame_rate <= 0) {
        frame_array.frame_rate = 30.0; // Fallback to 30 FPS
    }
    
    // Allocate initial array for frames
    int allocated_frames = 10;
    frame_array.frames = (AVFrame **)malloc(sizeof(AVFrame *) * allocated_frames);

    // Read frames
    while (av_read_frame(format_ctx, &packet) >= 0) {
        if (packet.stream_index == video_stream_index) {
            ret = avcodec_send_packet(codec_ctx, &packet);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet to decoder\n");
                av_packet_unref(&packet);
                continue;
            }

            while ((ret = avcodec_receive_frame(codec_ctx, frame = av_frame_alloc())) >= 0) {
                // Store the frame
                if (frame_array.frame_count >= allocated_frames) {
                    allocated_frames *= 2;
                    frame_array.frames = (AVFrame **)realloc(frame_array.frames, sizeof(AVFrame *) * allocated_frames);
                }
                frame_array.frames[frame_array.frame_count++] = frame;

                // Allocate a new frame for the next decode
                frame = av_frame_alloc();
            }

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_unref(&packet);
                continue;
            }
        }
        av_packet_unref(&packet);
    }

    // Clean up
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);

    return frame_array;
}

void freeFrameArray(FrameArray *frame_array) {
    for (int i = 0; i < frame_array->frame_count; i++) {
        av_frame_free(&frame_array->frames[i]);
    }
    free(frame_array->frames);
    frame_array->frames = NULL;
    frame_array->frame_count = 0;
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