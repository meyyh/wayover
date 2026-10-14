CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pedantic -std=c11
LIBS = -lwayland-client -lm -lavcodec -lavformat -lavutil -lswscale -lxkbcommon

SRCS = client.c ffmpeg.c xdg-shell-protocol.c wlr-layer-shell-unstable-v1-protocol.c
OBJS = $(SRCS:.c=.o)
TARGET = client

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
