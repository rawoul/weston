#ifndef ELA_WAYLAND_H_
#define ELA_WAYLAND_H_

#include <wayland-server.h>

struct ela_el *ela_wayland_create(struct wl_event_loop *loop);

#endif
