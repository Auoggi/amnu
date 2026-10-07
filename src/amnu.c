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
#include <stdlib.h>

#include <sys/mman.h>
#include <fcft/fcft.h>

#include <xkbcommon/xkbcommon.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <wlr-layer-shell-unstable-v1-client-protocol.h>

#include "config.h"

struct wl_shm *shm;
struct wl_seat *seat;
struct wl_display *display;
struct wl_keyboard *keyboard;
struct wl_compositor *compositor;
struct zwlr_layer_shell_v1 *zwlr_layer_shell;

struct xkb_context *xkb_context;
struct xkb_keymap *xkb_keymap;
struct xkb_state *xkb_state;

struct fcft_font *fcft_font = NULL;

struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;

int w, h;

char text[BUFSIZ] = "";
int cursor = 0;

void cleanup() {
  if(display) wl_display_disconnect(display);
}

void die(char *msg) {
  fprintf(stderr, "%s\n", msg);
  cleanup();
  exit(1);
}

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
  uint32_t stride = w * 4;
  int shm_pool_size = h * stride;

  int fd = allocate_shm_file(shm_pool_size);
  uint8_t *pool_data = mmap(NULL, shm_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, shm_pool_size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);

  pixman_image_t *pix = NULL;
  pix = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, w, h, (void*) pool_data, stride);
  if(pix == NULL) {
    die("error: failed to create pixman image");
  }

  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[0][1], 1, (pixman_rectangle16_t []){{0, 0, w, h}});

  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{0, 0, w, 4}}); // top
  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{0, 0, 4, h}}); // left
  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{w - 4, 0, 4, h}}); // right
  pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &colors[1][1], 1, (pixman_rectangle16_t []){{0, h - 4, w, 4}}); // bottom

  pixman_image_t *color = pixman_image_create_solid_fill(&colors[0][0]);

  render_chars(text, strlen(text), borderpx + 4, borderpx + 4, pix, color);

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

void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
  if(format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    die("Keyboard does not support xkb_v1");
  }

  char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if(map_shm == MAP_FAILED) {
    die("Keymap mmap failed");
  }

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map_shm, size);
  close(fd);

  if(xkb_state) xkb_state_unref(xkb_state);
  if(xkb_keymap) xkb_keymap_unref(xkb_keymap);
  xkb_state = xkb_state_new(keymap);
  xkb_keymap = keymap;
}

void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
  die("Keyboard left surface");
}

void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  if(state == 0) return;

  uint8_t control = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

  int keycode = key + 8;

  xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, keycode);
  if(sym == XKB_KEY_Escape) { 
    cleanup();
    exit(0);
  }
  if(sym == XKB_KEY_Return) {
    printf("%s", text);
    cleanup();
    exit(0);
  }

  bool keybind = false;
  if(sym == XKB_KEY_BackSpace) {
    text[--cursor] = 0;
    keybind = true;
  }

  if(!keybind && !control) {
    char buf[128];
    if(xkb_state_key_get_utf8(xkb_state, keycode, buf, sizeof(buf))) {
      size_t sl = strnlen(buf, 8);
      memcpy(&text[cursor], buf, sl);
      cursor += sl;
    }
  }

  render();
}

void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
  xkb_state_update_mask(xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {}
void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {}

const struct wl_keyboard_listener wl_keyboard_listener = {
  .keymap = wl_keyboard_keymap,
  .enter = wl_keyboard_enter,
  .leave = wl_keyboard_leave,
  .key = wl_keyboard_key,
  .modifiers = wl_keyboard_modifiers,
  .repeat_info = wl_keyboard_repeat_info,
};

void wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
  if(!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) return;

  keyboard = wl_seat_get_keyboard(seat);
  wl_keyboard_add_listener(keyboard, &wl_keyboard_listener, NULL);
}

void wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {}

const struct wl_seat_listener wl_seat_listener = {
  .capabilities = wl_seat_capabilities,
  .name = wl_seat_name,
};

void zwlr_layer_surface_v1_configure(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1, uint32_t serial, uint32_t width, uint32_t height) {
  zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface_v1, serial);

  w = width;
  h = height;
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

  if(strcmp(interface, wl_seat_interface.name) == 0) {
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
    wl_seat_add_listener(seat, &wl_seat_listener, NULL);
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
  xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  display = wl_display_connect(NULL);
  if(!display) {
    die("Failed to connect to Wayland display");
  }

  const char *name[] = { font };
  fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_NONE);
  fcft_font = fcft_from_name2(1, name, NULL, NULL);

  if(!fcft_font) {
    die("Font failed");
  }

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if(wl_display_roundtrip(display) < 0) {
    die("Roundtrip failed");
  }

  surface = wl_compositor_create_surface(compositor);

  struct wl_region *input_region = wl_compositor_create_region(compositor);
  wl_surface_set_input_region(surface, input_region);
  wl_region_destroy(input_region);

  layer_surface = zwlr_layer_shell_v1_get_layer_surface(zwlr_layer_shell, surface, NULL, 3, "amnu");

  zwlr_layer_surface_v1_set_size(layer_surface, 500 + 2*borderpx, 300 + 2*borderpx); // TODO: calculate from input
  zwlr_layer_surface_v1_set_anchor(layer_surface, 15);
  zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);

  zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
  wl_surface_commit(surface);

  if(wl_display_dispatch(display) == -1) {
    die("First dispatch failed");
  }

  render();

  while(wl_display_dispatch(display) != -1) { /* left blank */ }

  cleanup();
  return 0;
}
