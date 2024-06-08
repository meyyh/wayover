#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    /* State */
 	float offset;
	uint32_t last_frame;
    //image
    uint8_t *img;
    int img_width;
    int img_height;
    int img_channels;
    int img_opacity;
    uint8_t *img2;
    int img2_width;
    int img2_height;
    int img2_channels;
    int img2_opacity;
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

uint8_t *change_opacity(uint8_t *image, int height, int width, int percentage) {
    if (percentage < 0) {
        percentage = 0;
    } else if (percentage > 100) {
        percentage = 100;
    }
    
    int opacity = (percentage * 255) / 100;
    
    if (opacity < 0) {
        opacity = 0;
    } else if (opacity > 255) {
        opacity = 255;
    }
    
    char hex[2];
    sprintf(hex, "%x", opacity);
    printf("%s\n", hex);
    
    for (int i = 0; i < width * height; i++) {
        image[i * 4 + 3] = opacity;  // Set the alpha channel of each pixel
    }
    return image;
}

static struct wl_buffer *
draw_frame(struct client_state *state)
{
    const int img_width = state->img_width, img_height = state->img_height;
    int img_stride = img_width * state->img_channels;
    int img_size = img_height * img_stride;

    int height = 720;
    int width = 1280;
    int stride = width * 4;
    int size = height * stride;

    if(img_size > size){
        printf("image to big\n");
    }

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
        close(fd);
    }

    uint32_t *data = mmap(NULL, size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }


    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, width * 4, WL_SHM_FORMAT_ABGR8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    int img_x = 500;
    int img_y = 50;
    int img2_x = 0;
    int img2_y = 50;

    state->img2_opacity = 30;

    if(state->img2_opacity != -1){
        state->img2 = change_opacity(state->img2, state->img2_height, state->img2_width, state->img2_opacity);
    }
    else if(state->img_opacity != -1){
        state->img = change_opacity(state->img, state->img_height, state->img_width, state->img_opacity);
    }
    memset(data, 0, size);
  
    for (int y = 0; y < img_height; y++) {
        uint32_t *dst_row_start = data + (img_y + y) * width + img_x;
        uint32_t *src_row_start = (uint32_t *)(state->img + y * img_width * 4);
        memcpy(dst_row_start, src_row_start, img_width * 4);
        // No need for memset as the initial memset already set all pixels to blank
    }

    for (int y = 0; y < state->img2_height; y++) {
        uint32_t *dst_row_start = data + (img2_y + y) * width + img2_x;
        uint32_t *src_row_start = (uint32_t *)(state->img2 + y * state->img2_width * 4);
        memcpy(dst_row_start, src_row_start, state->img2_width * 4);
        // No need for memset as the initial memset already set all pixels to blank
    }


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

    struct wl_buffer *buffer = draw_frame(state);
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
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	/* Destroy this callback */
	wl_callback_destroy(cb);

	/* Request another frame */
	struct client_state *state = data;
	cb = wl_surface_frame(state->wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

	/* Submit a frame for this event */
	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(state->wl_surface);
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done,
};


static void
registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = wl_registry_bind(
                wl_registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = wl_registry_bind(
                wl_registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(
                wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base,
                &xdg_wm_base_listener, state);
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

int
main(int argc, char *argv[])
{
    struct client_state state = { 0 };
    state.wl_display = wl_display_connect(NULL);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
    wl_surface_commit(state.wl_surface);

    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);
    
    state.img = stbi_load(argv[1], &state.img_width, &state.img_height, &state.img_channels, 0);
    if(state.img == NULL) {
        printf("Error in loading the image\n");
        exit(1);
    }

    state.img2 = stbi_load(argv[2], &state.img2_width, &state.img2_height, &state.img2_channels, 0);
    if(state.img2 == NULL) {
        printf("Error in loading the image\n");
        exit(1);
    }
    
      
    while (wl_display_dispatch(state.wl_display)) {
        /* This space deliberately left blank */
    }
    stbi_image_free(state.img);
    stbi_image_free(state.img2);
    return 0;
}
//gcc -o client client.c xdg-shell-protocol.c -lwayland-client -lm