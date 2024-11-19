#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

typedef struct {
    AVFrame **frames;
    int frame_count;
    int frame_rate;
} FrameArray;

FrameArray getFrames(const char *inputfile);
AVFrame *toARGB(AVFrame *frame);