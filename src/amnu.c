#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <fcft/fcft.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <wlr-layer-shell-unstable-v1-client-protocol.h>

#include "config.h"

struct wl_shm *shm;
struct wl_compositor *compositor;
struct zwlr_layer_shell_v1 *zwlr_layer_shell;

struct fcft_font *fcft_font = NULL;

struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;

int amnu_width, amnu_height;

// https://wayland-book.com/surfaces/shared-memory.html
// ---
void randname(char *buf) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long r = ts.tv_nsec;
  for (int i = 0; i < 6; ++i) {
    buf[i] = 'A'+(r&15)+(r&16)*2;
    r >>= 5;
  }
}

int create_shm_file(void) {
  int retries = 100;
  do {
    char name[] = "/wl_shm-XXXXXX";
    randname(name + sizeof(name) - 7);
    --retries;
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if(fd >= 0) {
      shm_unlink(name);
      return fd;
    }
  } while (retries > 0 && errno == EEXIST);
  return -1;
}

int allocate_shm_file(size_t size) {
  int fd = create_shm_file();
  if(fd < 0) return -1;

  int ret;
  do {
    ret = ftruncate(fd, size);
  } while (ret < 0 && errno == EINTR);

  if(ret < 0) {
    close(fd);
    return -1;
  }
  return fd;
}
// ---

void render_chars(const char *chars, size_t len, int x, int y, pixman_image_t *pix, pixman_image_t *color) {
  const struct fcft_glyph *glyphs[len];
  long kern[len];
  int text_width = 0;

  for(size_t i = 0; i < len; i++) {
    glyphs[i] = fcft_rasterize_char_utf32(fcft_font, chars[i], FCFT_SUBPIXEL_DEFAULT);
    if(glyphs[i] == NULL) continue;

    kern[i] = 0;
    if(i > 0) {
      long x_kern;
      if(fcft_kerning(fcft_font, chars[i - 1], chars[i], &x_kern, NULL)) kern[i] = x_kern;
    }

    text_width += kern[i] + glyphs[i]->advance.x;
  }

  int cx = x;
  for(size_t i = 0; i < len; i++) {
    const struct fcft_glyph *g = glyphs[i];
    if(g == NULL) continue;
    cx += kern[i];

    pixman_image_composite32(PIXMAN_OP_OVER, color, g->pix, pix, 0, 0, 0, 0, cx + g->x, y + fcft_font->ascent - g->y, g->width, g->height);

    cx += g->advance.x;
  }
}

void render() {
  int w = amnu_width, h = amnu_height;

  uint32_t stride = w * 4;
  int shm_pool_size = h * stride;

  int fd = allocate_shm_file(shm_pool_size);
  uint8_t *pool_data = mmap(NULL, shm_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, shm_pool_size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);

  pixman_image_t *pix = NULL;
  pix = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, w, h, (void*) pool_data, stride);
  if(pix == NULL) {
    fprintf(stderr, "error: failed to create pixman image\n");
    return;
  }

  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[0][1], 1, (pixman_rectangle16_t []){{0, 0, w, h}});

  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{0, 0, w, 4}}); // top
  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{0, 0, 4, h}}); // left
  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{w - 4, 0, 4, h}}); // right
  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{0, h - 4, w, 4}}); // bottom

  pixman_image_t *color = pixman_image_create_solid_fill(&colors[0][0]);

  int y = (h - 2*borderpx - fcft_font->height) / 2;
  render_chars("Hello world!", 12, borderpx + 4, y, pix, color);

  pixman_image_unref(color);

  wl_surface_attach(surface, buf, 0, 0);
  wl_surface_offset(surface, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, w, h);
  wl_surface_commit(surface);

  wl_shm_pool_destroy(pool); pool = NULL;
  close(fd); fd = -1;

  pixman_image_unref(pix);
  wl_buffer_destroy(buf);
  munmap(pool_data, shm_pool_size);
}

void zwlr_layer_surface_v1_configure(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1, uint32_t serial, uint32_t width, uint32_t height) {
  zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface_v1, serial);

  amnu_width = width;
  amnu_height = height;
}

void zwlr_layer_surface_v1_closed(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1) {}

const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  .configure = zwlr_layer_surface_v1_configure,
  .closed = zwlr_layer_surface_v1_closed,
};

void wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  if(strcmp(interface, wl_shm_interface.name) == 0) {
    shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  }

  if(strcmp(interface, wl_compositor_interface.name) == 0) {
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
  }

  if(strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    zwlr_layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
  }
}

void wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

const struct wl_registry_listener registry_listener = {
  .global = wl_registry_global,
  .global_remove = wl_registry_global_remove,
};

int main() {
  struct wl_display *display = wl_display_connect(NULL);
  if(display == NULL) {
    fprintf(stderr, "Failed to connect to Wayland display.\n");
    return 1;
  }

  const char *name[] = { font };
  fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_DEBUG);
  fcft_font = fcft_from_name2(1, name, NULL, NULL);

  if(fcft_font == NULL) {
    fprintf(stderr, "Font failed\n");
    return 1;
  }

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if(wl_display_roundtrip(display) < 0) {
    fprintf(stderr, "Roundtrip failed\n");
    return 1;
  }

  surface = wl_compositor_create_surface(compositor);

  struct wl_region *input_region = wl_compositor_create_region(compositor);
  wl_surface_set_input_region(surface, input_region);
  wl_region_destroy(input_region);

  layer_surface = zwlr_layer_shell_v1_get_layer_surface(zwlr_layer_shell, surface, NULL, 3, "amnu");

  zwlr_layer_surface_v1_set_size(layer_surface, 500 + 2*borderpx, 300 + 2*borderpx);
  zwlr_layer_surface_v1_set_anchor(layer_surface, 15);
  zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);

  zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
  wl_surface_commit(surface);

  if(wl_display_dispatch(display) == -1) {
    fprintf(stderr, "First dispatch failed.\n");
  }

  render();

  while(wl_display_dispatch(display) != -1) { /* left blank */ }

  wl_display_disconnect(display);
  return 0;
}
