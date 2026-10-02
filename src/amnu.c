#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include <stdio.h>
#include <string.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <wlr-layer-shell-unstable-v1-client-protocol.h>

#include "config.h"

struct wl_shm *shm;
struct wl_compositor *compositor;
struct zwlr_layer_shell_v1 *zwlr_layer_shell;

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

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if(wl_display_roundtrip(display) < 0) {
    fprintf(stderr, "Roundtrip failed\n");
    return 1;
  }

  while(wl_display_dispatch(display) != -1) { /* left blank */ }

  wl_display_disconnect(display);
  return 0;
}
