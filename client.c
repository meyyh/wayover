#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>

#include "ffmpeg.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define NUM_BUFFERS 2
#define MOVE_STEP 15

/* Shared memory buffer structure for double-buffering */
struct shm_buffer {
    struct wl_buffer *wl_buffer;
    uint32_t *data;
    size_t size;
    bool busy;
    int prev_x;
    int prev_y;
    int prev_w;
    int prev_h;
    bool has_prev;
};

/* Application state */
struct client_state {
    /* Wayland globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct wl_seat *wl_seat;
    struct wl_output *wl_output;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct xdg_wm_base *xdg_wm_base;

    /* Wayland objects */
    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_keyboard *wl_keyboard;

    /* Canvas dimensions */
    int width;
    int height;
    int output_width;
    int output_height;

    /* Persistent SHM double buffers */
    struct shm_buffer buffers[NUM_BUFFERS];
    int active_buffer_idx;
    int shm_pool_fd;
    size_t shm_pool_size;
    void *shm_pool_data;

    /* XKB Keyboard state */
    struct xkb_state *xkb_state;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;

    /* Video player */
    VideoDecoder vd;
    const char *video_path;
    int img_x;
    int img_y;
    double frame_duration;
    struct timespec last_frame_time;

    /* Options & flags */
    bool running;
    bool configured;
    bool force_xdg;
    bool passthrough_clicks;
    bool user_x_set;
    bool user_y_set;
};

/* Shared memory helper functions */
static void randname(char *buf) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

static int create_shm_file(void) {
    int retries = 100;
    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static int allocate_shm_file(size_t size) {
    int fd = create_shm_file();
    if (fd < 0) return -1;
    int ret;
    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    struct shm_buffer *buf = (struct shm_buffer *)data;
    if (buf) {
        buf->busy = false;
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = wl_buffer_release,
};

static int init_buffers(struct client_state *state) {
    int stride = state->width * 4;
    size_t buffer_size = (size_t)stride * state->height;
    state->shm_pool_size = buffer_size * NUM_BUFFERS;

    state->shm_pool_fd = allocate_shm_file(state->shm_pool_size);
    if (state->shm_pool_fd < 0) {
        fprintf(stderr, "Error: Failed to allocate shared memory file of size %zu\n", state->shm_pool_size);
        return -1;
    }

    state->shm_pool_data = mmap(NULL, state->shm_pool_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                state->shm_pool_fd, 0);
    if (state->shm_pool_data == MAP_FAILED) {
        fprintf(stderr, "Error: mmap failed on shared memory file\n");
        close(state->shm_pool_fd);
        state->shm_pool_fd = -1;
        return -1;
    }

    // Zero out buffer initially (fully transparent)
    memset(state->shm_pool_data, 0, state->shm_pool_size);

    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, state->shm_pool_fd, state->shm_pool_size);
    if (!pool) {
        fprintf(stderr, "Error: Failed to create wl_shm_pool\n");
        return -1;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        state->buffers[i].size = buffer_size;
        state->buffers[i].data = (uint32_t *)((uint8_t *)state->shm_pool_data + (i * buffer_size));
        state->buffers[i].busy = false;
        state->buffers[i].has_prev = false;

        state->buffers[i].wl_buffer = wl_shm_pool_create_buffer(
            pool,
            i * buffer_size,
            state->width,
            state->height,
            stride,
            WL_SHM_FORMAT_ARGB8888
        );
        wl_buffer_add_listener(state->buffers[i].wl_buffer, &buffer_listener, &state->buffers[i]);
    }

    wl_shm_pool_destroy(pool);
    return 0;
}

static void free_buffers(struct client_state *state) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (state->buffers[i].wl_buffer) {
            wl_buffer_destroy(state->buffers[i].wl_buffer);
            state->buffers[i].wl_buffer = NULL;
        }
    }
    if (state->shm_pool_data && state->shm_pool_data != MAP_FAILED) {
        munmap(state->shm_pool_data, state->shm_pool_size);
        state->shm_pool_data = NULL;
    }
    if (state->shm_pool_fd >= 0) {
        close(state->shm_pool_fd);
        state->shm_pool_fd = -1;
    }
}

/* Pick next available buffer */
static struct shm_buffer *get_next_buffer(struct client_state *state) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        int idx = (state->active_buffer_idx + i) % NUM_BUFFERS;
        if (!state->buffers[idx].busy) {
            state->active_buffer_idx = (idx + 1) % NUM_BUFFERS;
            return &state->buffers[idx];
        }
    }
    // If all buffers are busy, use the oldest one
    state->active_buffer_idx = (state->active_buffer_idx + 1) % NUM_BUFFERS;
    return &state->buffers[state->active_buffer_idx];
}

/* Forward declaration for surface frame callback listener */
static const struct wl_callback_listener frame_listener;

/* Render current frame into double-buffer and submit damage */
static void render_and_commit(struct client_state *state) {
    struct shm_buffer *buf = get_next_buffer(state);
    if (!buf || !buf->data) return;

    int vw = state->vd.width;
    int vh = state->vd.height;
    int cw = state->width;
    int ch = state->height;

    // Boundary clipping for blitting video onto canvas
    int src_x = 0, src_y = 0;
    int dst_x = state->img_x, dst_y = state->img_y;
    int copy_w = vw, copy_h = vh;

    if (dst_x < 0) {
        src_x = -dst_x;
        copy_w += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y = -dst_y;
        copy_h += dst_y;
        dst_y = 0;
    }
    if (dst_x + copy_w > cw) {
        copy_w = cw - dst_x;
    }
    if (dst_y + copy_h > ch) {
        copy_h = ch - dst_y;
    }

    // If buffer had previous video content, clear only the previous area if it moved
    if (buf->has_prev) {
        if (buf->prev_x != dst_x || buf->prev_y != dst_y ||
            buf->prev_w != copy_w || buf->prev_h != copy_h) {
            for (int y = 0; y < buf->prev_h; y++) {
                uint32_t *row = buf->data + (buf->prev_y + y) * cw + buf->prev_x;
                memset(row, 0, (size_t)buf->prev_w * 4);
            }
        }
    }

    // Direct decode into SHM buffer when fully inside, fallback to clipped copy when touching edges
    bool fully_inside = (dst_x >= 0 && dst_y >= 0 && dst_x + vw <= cw && dst_y + vh <= ch);
    if (fully_inside) {
        uint8_t *dst_ptr = (uint8_t *)(buf->data + dst_y * cw + dst_x);
        video_decoder_read_next_frame(&state->vd, dst_ptr, cw * 4);
    } else {
        if (video_decoder_read_next_frame(&state->vd, NULL, 0) == 0 && state->vd.bgra_buffer) {
            if (copy_w > 0 && copy_h > 0) {
                for (int y = 0; y < copy_h; y++) {
                    uint32_t *dst_row = buf->data + (dst_y + y) * cw + dst_x;
                    const uint32_t *src_row = (const uint32_t *)(state->vd.bgra_buffer + ((src_y + y) * vw + src_x) * 4);
                    memcpy(dst_row, src_row, (size_t)copy_w * 4);
                }
            }
        }
    }

    // Compute damage rectangle (union of previous and current rectangles)
    int damage_x = dst_x;
    int damage_y = dst_y;
    int damage_w = copy_w;
    int damage_h = copy_h;

    if (buf->has_prev) {
        int min_x = dst_x < buf->prev_x ? dst_x : buf->prev_x;
        int min_y = dst_y < buf->prev_y ? dst_y : buf->prev_y;
        int max_x = (dst_x + copy_w) > (buf->prev_x + buf->prev_w) ? (dst_x + copy_w) : (buf->prev_x + buf->prev_w);
        int max_y = (dst_y + copy_h) > (buf->prev_y + buf->prev_h) ? (dst_y + copy_h) : (buf->prev_y + buf->prev_h);
        damage_x = min_x;
        damage_y = min_y;
        damage_w = max_x - min_x;
        damage_h = max_y - min_y;
    }

    buf->prev_x = dst_x;
    buf->prev_y = dst_y;
    buf->prev_w = copy_w;
    buf->prev_h = copy_h;
    buf->has_prev = true;
    buf->busy = true;

    wl_surface_attach(state->wl_surface, buf->wl_buffer, 0, 0);
    wl_surface_damage_buffer(state->wl_surface, damage_x, damage_y, damage_w, damage_h);

    struct wl_callback *cb = wl_surface_frame(state->wl_surface);
    wl_callback_add_listener(cb, &frame_listener, state);
    wl_surface_commit(state->wl_surface);
}

static void wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    wl_callback_destroy(cb);
    struct client_state *state = (struct client_state *)data;
    if (!state->running) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (now.tv_sec - state->last_frame_time.tv_sec) +
                     (now.tv_nsec - state->last_frame_time.tv_nsec) / 1e9;

    // Small epsilon (4ms) to account for compositor vblank jitter
    if (elapsed >= (state->frame_duration - 0.004)) {
        state->last_frame_time = now;
        render_and_commit(state);
        return;
    }

    // Between video frames: keep vsync sync alive without new buffer
    struct wl_callback *next_cb = wl_surface_frame(state->wl_surface);
    wl_callback_add_listener(next_cb, &frame_listener, state);
    wl_surface_commit(state->wl_surface);
}

static const struct wl_callback_listener frame_listener = {
    .done = wl_surface_frame_done,
};

/* Keyboard handling */
static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                               uint32_t format, int32_t fd, uint32_t size) {
    (void)wl_keyboard;
    struct client_state *state = (struct client_state *)data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_shm == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        state->xkb_context, map_shm,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    munmap(map_shm, size);
    close(fd);

    if (keymap) {
        struct xkb_state *xstate = xkb_state_new(keymap);
        if (state->xkb_keymap) xkb_keymap_unref(state->xkb_keymap);
        if (state->xkb_state) xkb_state_unref(state->xkb_state);
        state->xkb_keymap = keymap;
        state->xkb_state = xstate;
    }
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                              uint32_t serial, struct wl_surface *surface,
                              struct wl_array *keys) {
    (void)data; (void)wl_keyboard; (void)serial; (void)surface; (void)keys;
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
                              uint32_t serial, struct wl_surface *surface) {
    (void)data; (void)wl_keyboard; (void)serial; (void)surface;
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                            uint32_t serial, uint32_t time, uint32_t key, uint32_t key_state) {
    (void)wl_keyboard; (void)serial; (void)time;
    struct client_state *state = (struct client_state *)data;

    if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (state->xkb_state) {
        sym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);
    }

    // Handle WASD and Arrow keys
    if (sym == XKB_KEY_w || sym == XKB_KEY_W || sym == XKB_KEY_Up || key == 17 || key == 103) {
        state->img_y -= MOVE_STEP;
    } else if (sym == XKB_KEY_s || sym == XKB_KEY_S || sym == XKB_KEY_Down || key == 31 || key == 108) {
        state->img_y += MOVE_STEP;
    } else if (sym == XKB_KEY_a || sym == XKB_KEY_A || sym == XKB_KEY_Left || key == 30 || key == 105) {
        state->img_x -= MOVE_STEP;
    } else if (sym == XKB_KEY_d || sym == XKB_KEY_D || sym == XKB_KEY_Right || key == 32 || key == 106) {
        state->img_x += MOVE_STEP;
    } else if (sym == XKB_KEY_Escape || sym == XKB_KEY_q || sym == XKB_KEY_Q || key == 1 || key == 16) {
        state->running = false;
    }
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, uint32_t mods_depressed,
                                  uint32_t mods_latched, uint32_t mods_locked,
                                  uint32_t group) {
    (void)wl_keyboard; (void)serial;
    struct client_state *state = (struct client_state *)data;
    if (state->xkb_state) {
        xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    }
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                    int32_t rate, int32_t delay) {
    (void)data; (void)wl_keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

static void wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct client_state *state = (struct client_state *)data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

    if (has_keyboard && !state->wl_keyboard) {
        state->wl_keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(state->wl_keyboard, &keyboard_listener, state);
    } else if (!has_keyboard && state->wl_keyboard) {
        wl_keyboard_release(state->wl_keyboard);
        state->wl_keyboard = NULL;
    }
}

static void wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    (void)data; (void)wl_seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

/* Output listener to detect monitor resolution */
static void wl_output_geometry(void *data, struct wl_output *wl_output,
                               int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
                               int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data; (void)wl_output; (void)x; (void)y; (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void wl_output_mode(void *data, struct wl_output *wl_output,
                           uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)wl_output; (void)refresh;
    struct client_state *state = (struct client_state *)data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        state->output_width = width;
        state->output_height = height;
    }
}

static void wl_output_done(void *data, struct wl_output *wl_output) {
    (void)data; (void)wl_output;
}

static void wl_output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    (void)data; (void)wl_output; (void)factor;
}

static void wl_output_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)data; (void)wl_output; (void)name;
}

static void wl_output_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data; (void)wl_output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = wl_output_geometry,
    .mode = wl_output_mode,
    .done = wl_output_done,
    .scale = wl_output_scale,
    .name = wl_output_name,
    .description = wl_output_description,
};

/* Layer shell surface listener */
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct client_state *state = (struct client_state *)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    if (width > 0) state->width = width;
    if (height > 0) state->height = height;

    if (!state->configured) {
        if (!state->user_x_set) {
            state->img_x = (state->width - state->vd.width) / 2;
        }
        if (!state->user_y_set) {
            state->img_y = (state->height - state->vd.height) / 2;
        }
        init_buffers(state);
        state->configured = true;

        render_and_commit(state);
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    struct client_state *state = (struct client_state *)data;
    state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* XDG Shell surface listener */
static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct client_state *state = (struct client_state *)data;
    xdg_surface_ack_configure(xdg_surface, serial);

    if (!state->configured) {
        if (!state->user_x_set) {
            state->img_x = (state->width - state->vd.width) / 2;
        }
        if (!state->user_y_set) {
            state->img_y = (state->height - state->vd.height) / 2;
        }
        init_buffers(state);
        state->configured = true;

        render_and_commit(state);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    (void)xdg_toplevel; (void)states;
    struct client_state *state = (struct client_state *)data;
    if (width > 0) state->width = width;
    if (height > 0) state->height = height;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    (void)xdg_toplevel;
    struct client_state *state = (struct client_state *)data;
    state->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

/* Registry listener */
static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    struct client_state *state = (struct client_state *)data;

    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(state->wl_seat, &seat_listener, state);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!state->wl_output) {
            state->wl_output = wl_registry_bind(wl_registry, name, &wl_output_interface, 4);
            wl_output_add_listener(state->wl_output, &output_listener, state);
        }
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);
    }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry, uint32_t name) {
    (void)data; (void)wl_registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options] <video_file> [x] [y]\n\n", prog_name);
    printf("Positional Arguments:\n");
    printf("  video_file         Path to transparent video (e.g. ProRes 4444 .mov)\n");
    printf("  x                  Initial X coordinate (-1 to center, default: center)\n");
    printf("  y                  Initial Y coordinate (-1 to center, default: center)\n\n");
    printf("Options:\n");
    printf("  -x <val>           Set initial X coordinate (-1 for center)\n");
    printf("  -y <val>           Set initial Y coordinate (-1 for center)\n");
    printf("  -w <width>         Canvas width (default: auto-detected monitor width)\n");
    printf("  -h <height>        Canvas height (default: auto-detected monitor height)\n");
    printf("  --xdg              Force XDG-shell window instead of layer-shell overlay\n");
    printf("  --passthrough      Pass mouse clicks through overlay to background windows\n");
    printf("  --help             Show this help message\n\n");
    printf("Controls:\n");
    printf("  W / Up Arrow       Move overlay up\n");
    printf("  S / Down Arrow     Move overlay down\n");
    printf("  A / Left Arrow     Move overlay left\n");
    printf("  D / Right Arrow    Move overlay right\n");
    printf("  Q / Escape         Quit application\n");
}

int main(int argc, char *argv[]) {
    struct client_state state;
    memset(&state, 0, sizeof(state));
    state.running = true;
    state.shm_pool_fd = -1;

    int positional_arg = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            if (i + 1 < argc && argv[i][1] == 'h' && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                // Was -h <height>
                state.height = atoi(argv[++i]);
                continue;
            }
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--xdg") == 0) {
            state.force_xdg = true;
        } else if (strcmp(argv[i], "--passthrough") == 0) {
            state.passthrough_clicks = true;
        } else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            state.img_x = atoi(argv[++i]);
            state.user_x_set = (state.img_x != -1);
        } else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) {
            state.img_y = atoi(argv[++i]);
            state.user_y_set = (state.img_y != -1);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            state.width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            state.height = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            if (positional_arg == 0) {
                state.video_path = argv[i];
                positional_arg++;
            } else if (positional_arg == 1) {
                state.img_x = atoi(argv[i]);
                state.user_x_set = (state.img_x != -1);
                positional_arg++;
            } else if (positional_arg == 2) {
                state.img_y = atoi(argv[i]);
                state.user_y_set = (state.img_y != -1);
                positional_arg++;
            }
        }
    }

    if (!state.video_path) {
        fprintf(stderr, "Error: No video file specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Open video decoder
    if (video_decoder_open(&state.vd, state.video_path) < 0) {
        return 1;
    }
    printf("Loaded '%s': %dx%d @ %.2f fps\n",
           state.video_path, state.vd.width, state.vd.height, state.vd.frame_rate);

    state.frame_duration = 1.0 / state.vd.frame_rate;
    clock_gettime(CLOCK_MONOTONIC, &state.last_frame_time);

    // Connect to Wayland
    state.wl_display = wl_display_connect(NULL);
    if (!state.wl_display) {
        fprintf(stderr, "Error: Failed to connect to Wayland display.\n");
        video_decoder_close(&state.vd);
        return 1;
    }

    state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    wl_registry_add_listener(state.wl_registry, &registry_listener, &state);

    // Initial roundtrip to bind globals and query output size
    wl_display_roundtrip(state.wl_display);
    wl_display_roundtrip(state.wl_display);

    // Auto-detect resolution if not specified
    if (state.width <= 0) {
        state.width = (state.output_width > 0) ? state.output_width : 1920;
    }
    if (state.height <= 0) {
        state.height = (state.output_height > 0) ? state.output_height : 1080;
    }

    // Create main surface
    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    if (!state.wl_surface) {
        fprintf(stderr, "Error: Failed to create Wayland surface.\n");
        goto cleanup;
    }

    // If mouse click passthrough is requested, set empty input region
    if (state.passthrough_clicks) {
        struct wl_region *empty_region = wl_compositor_create_region(state.wl_compositor);
        wl_surface_set_input_region(state.wl_surface, empty_region);
        wl_region_destroy(empty_region);
    }

    // Use wlr-layer-shell if available and not forced to XDG
    if (state.layer_shell && !state.force_xdg) {
        state.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
            state.layer_shell,
            state.wl_surface,
            state.wl_output,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
            "wayover"
        );
        zwlr_layer_surface_v1_add_listener(state.layer_surface, &layer_surface_listener, &state);

        // Anchor to all 4 edges to span screen
        uint32_t anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        zwlr_layer_surface_v1_set_anchor(state.layer_surface, anchors);
        zwlr_layer_surface_v1_set_size(state.layer_surface, state.width, state.height);
        zwlr_layer_surface_v1_set_exclusive_zone(state.layer_surface, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            state.layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
        );

        // Layer-shell requires initial commit without buffer
        wl_surface_commit(state.wl_surface);
    } else if (state.xdg_wm_base) {
        // Fallback to xdg_shell
        state.xdg_surface = xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
        xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);

        state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
        xdg_toplevel_add_listener(state.xdg_toplevel, &toplevel_listener, &state);
        xdg_toplevel_set_app_id(state.xdg_toplevel, "wayover");
        xdg_toplevel_set_title(state.xdg_toplevel, "Choo Choo");

        wl_surface_commit(state.wl_surface);
    } else {
        fprintf(stderr, "Error: Compositor does not support layer-shell or xdg-shell.\n");
        goto cleanup;
    }

    // Main event loop
    while (state.running && wl_display_dispatch(state.wl_display) != -1) {
        // Dispatch events
    }

cleanup:
    free_buffers(&state);
    video_decoder_close(&state.vd);

    if (state.layer_surface) zwlr_layer_surface_v1_destroy(state.layer_surface);
    if (state.xdg_toplevel) xdg_toplevel_destroy(state.xdg_toplevel);
    if (state.xdg_surface) xdg_surface_destroy(state.xdg_surface);
    if (state.wl_surface) wl_surface_destroy(state.wl_surface);
    if (state.wl_keyboard) wl_keyboard_release(state.wl_keyboard);
    if (state.xkb_state) xkb_state_unref(state.xkb_state);
    if (state.xkb_keymap) xkb_keymap_unref(state.xkb_keymap);
    if (state.xkb_context) xkb_context_unref(state.xkb_context);
    if (state.wl_output) wl_output_destroy(state.wl_output);
    if (state.layer_shell) zwlr_layer_shell_v1_destroy(state.layer_shell);
    if (state.xdg_wm_base) xdg_wm_base_destroy(state.xdg_wm_base);
    if (state.wl_seat) wl_seat_destroy(state.wl_seat);
    if (state.wl_shm) wl_shm_destroy(state.wl_shm);
    if (state.wl_compositor) wl_compositor_destroy(state.wl_compositor);
    if (state.wl_registry) wl_registry_destroy(state.wl_registry);
    if (state.wl_display) wl_display_disconnect(state.wl_display);

    return 0;
}