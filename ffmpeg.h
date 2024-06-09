#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

AVFrame *getFrame(const char *inputfile);