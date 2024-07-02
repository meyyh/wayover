#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

AVFrame *getFrame(const char *inputfile);
AVFrame *toARGB(AVFrame *frame);