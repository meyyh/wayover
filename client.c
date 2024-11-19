#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "ffmpeg.h"


/* Shared memory support code */
static void
randname(char *buf)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A'+(r&15)+(r&16)*2;
        r >>= 5;
    }
}

static int
create_shm_file(void)
{
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

static int
allocate_shm_file(size_t size)
{
    int fd = create_shm_file();
    if (fd < 0)
        return -1;
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

/* Wayland code */
struct client_state {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *wl_seat;
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_keyboard *wl_keyboard;
    int height;
    int width;
    //state
    struct xkb_state *xkb_state;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    // image
    char *img_path;
    int img_x;
    int img_y;
    int img_width;
    int img_height;
    FrameArray frame_array;
    struct timespec last_frame_time;
    double frame_duration; // In seconds
    int current_frame;     // Index of the current frame
};

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *
draw_frame(struct client_state *state, int frame_num)
{
    int width = state->width;
    int height = state->height;
    int stride = width * 4;
    int size = stride * height;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    
    /* Draw checkerboxed background 
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8)
                data[y * width + x] = 0xFF666666;
            else
                data[y * width + x] = 0xFFEEEEEE;
        }
    }*/

    AVFrame *frame = state->frame_array.frames[frame_num];
    //AVFrame *frame = getFrames(state->img_path);
    if (!frame) {
        fprintf(stderr, "Failed to get frame\n");
        munmap(data, size);
        return NULL;
    }

    frame = toARGB(frame);
    for (int y = 0; y < frame->height; y++) {
        uint32_t *dst_row_start = data + ((state->img_y + y) * width) + state->img_x;
        uint32_t *src_row_start = (uint32_t *)(frame->data[0] + (y * frame->linesize[0]));
        memcpy(dst_row_start, src_row_start, frame->width * 4);
    }
    av_frame_free(&frame);
    munmap(data, size);

    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}

static void
xdg_surface_configure(void *data,
        struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct client_state *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(state, 0);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static void
wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                   uint32_t format, int32_t fd, uint32_t size)
{
    struct client_state *client_state = data;
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    assert(map_shm != MAP_FAILED);

    struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_string(
                       client_state->xkb_context, map_shm,
                       XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size);
    close(fd);

    struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
    xkb_keymap_unref(client_state->xkb_keymap);
    xkb_state_unref(client_state->xkb_state);
    client_state->xkb_keymap = xkb_keymap;
    client_state->xkb_state = xkb_state;
}

static void
wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
                  uint32_t serial, struct wl_surface *surface,
                  struct wl_array *keys)
{
    struct client_state *client_state = data;
    //fprintf(stderr, "keyboard enter; keys pressed are:\n");
    uint32_t *key;
    wl_array_for_each(key, keys) {
        char buf[128];
        xkb_keysym_t sym = xkb_state_key_get_one_sym(client_state->xkb_state, *key + 8);
        xkb_keysym_get_name(sym, buf, sizeof(buf));
        //fprintf(stderr, "sym: %-12s (%d), ", buf, sym);
        xkb_state_key_get_utf8(client_state->xkb_state, *key + 8, buf, sizeof(buf));
        //fprintf(stderr, "utf8: '%s'\n", buf);
    }
}

void update_surface();

static void
wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct client_state *client_state = data;
    char buf[128];
    uint32_t keycode = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(client_state->xkb_state, keycode);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    const char *action = state == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release";
    //fprintf(stderr, "key %s: sym: %-12s (%d), ", action, buf, sym);
    xkb_state_key_get_utf8(client_state->xkb_state, keycode, buf, sizeof(buf));
    //fprintf(stderr, "utf8: '%s'\n", buf);
    printf("%d\n", key);
    
    if(key == 30 && action == "press"){
        client_state->img_x -= 10;
        //update_surface();
    } else if(key == 31 && action == "press"){
        client_state->img_y += 10;
    } else if(key == 32 && action == "press"){
        client_state->img_x += 10;
    } else if(key == 17 && action == "press"){
        client_state->img_y -= 10;
    }
}

static void
wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
                  uint32_t serial, struct wl_surface *surface)
{
    //fprintf(stderr, "keyboard leave\n");
}

static void
wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                      uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group)
{
    struct client_state *client_state = data;
    xkb_state_update_mask(client_state->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void
wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                        int32_t rate, int32_t delay)
{
    /* Left as an exercise for the reader */
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

static void
wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities)
{
    struct client_state *state = data;
    /*
    bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    if (have_pointer && state->wl_pointer == NULL) {
       state->wl_pointer = wl_seat_get_pointer(state->wl_seat);
        wl_pointer_add_listener(state->wl_pointer, &wl_pointer_listener, state);
    } 
    else if (!have_pointer && state->wl_pointer != NULL) {
        wl_pointer_release(state->wl_pointer);
        state->wl_pointer = NULL;
    }*/

    bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

    if (have_keyboard && state->wl_keyboard == NULL) {
        state->wl_keyboard = wl_seat_get_keyboard(state->wl_seat);
        wl_keyboard_add_listener(state->wl_keyboard, &wl_keyboard_listener, state);
    } 
    else if (!have_keyboard && state->wl_keyboard != NULL) {
        wl_keyboard_release(state->wl_keyboard);
        state->wl_keyboard = NULL;
    }
}

static void
wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
    //fprintf(stderr, "seat name: %s\n", name);
}

static const struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    /* Destroy this callback */
    wl_callback_destroy(cb);

    struct client_state *state = data;

    /* Calculate time elapsed since the last frame */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed_time = (now.tv_sec - state->last_frame_time.tv_sec) +
                          (now.tv_nsec - state->last_frame_time.tv_nsec) / 1e9;

    /* Sleep if we're ahead of schedule */
    if (elapsed_time < state->frame_duration) {
        struct timespec ts = {0};
        double sleep_time = state->frame_duration - elapsed_time;
        ts.tv_sec = (time_t)sleep_time;
        ts.tv_nsec = (long)((sleep_time - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }

    /* Update the timestamp for the last frame */
    clock_gettime(CLOCK_MONOTONIC, &state->last_frame_time);

    /* Submit the next frame */
    struct wl_buffer *buffer = draw_frame(state, state->current_frame);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(state->wl_surface);

    /* Advance to the next frame */
    state->current_frame = (state->current_frame + 1) % state->frame_array.frame_count;

    /* Request another frame callback */
    cb = wl_surface_frame(state->wl_surface);
    wl_callback_add_listener(cb, &wl_surface_frame_listener, state);
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) 
    {
        state->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
    } 
    else if (strcmp(interface, wl_compositor_interface.name) == 0) 
    {
        state->wl_compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
    } 
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base, &xdg_wm_base_listener, state);
    } 
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(state->wl_seat, &wl_seat_listener, state);
    }
}

static void
registry_global_remove(void *data,
        struct wl_registry *wl_registry, uint32_t name)
{
    /* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

void update_surface()
{
    struct client_state state = { 0 };
    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);
}

int
main(int argc, char *argv[])
{
    struct client_state state = { 0 };

    state.img_path = argv[1];

    state.height = 2160;
    state.width = 3840;

    if(atoi(argv[2]) == -1 || atoi(argv[3]) == -1)
    {
        state.img_x = (state.width / 2) - (state.img_width / 2);
        state.img_y = (state.height / 2) - (state.img_height / 2);
        printf("%s\n", state.img_x);
        printf("%s\n", state.img_y);
    }else{
        state.img_x = atoi(argv[2]);
        state.img_y = atoi(argv[3]);
    }

    state.frame_array = getFrames(argv[1]);

    if (state.frame_array.frames == NULL) {
        fprintf(stderr, "Failed to retrieve frames from the video.\n");
        return EXIT_FAILURE;
    }
    printf("Number of frames: %d\n", state.frame_array.frame_count);

    state.frame_duration = 1.0 / state.frame_array.frame_rate;
    state.current_frame = 0;
    clock_gettime(CLOCK_MONOTONIC, &state.last_frame_time);

    state.wl_display = wl_display_connect(NULL);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_set_title(state.xdg_toplevel, "Choo Choo");
    wl_surface_commit(state.wl_surface);

    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);
      
    while (wl_display_dispatch(state.wl_display)) {
        /* This space deliberately left blank */
    }

    return 0;
}
//gcc -o client client.c xdg-shell-protocol.c ffmpeg.c -lwayland-client -lm -lavcodec -lavformat -lavutil -lswscale -lxkbcommon
//./client ./sc3h2.mov 500 0