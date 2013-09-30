#ifndef ICE_RENDERER_H_
# define ICE_RENDERER_H_

#include <gdl_types.h>
#include "compositor.h"

void *ice_renderer_create_framebuffer(struct weston_renderer *renderer,
				      gdl_surface_info_t *surface_info,
				      gdl_uint8 *data);

void
ice_renderer_destroy_framebuffer(struct weston_renderer *renderer,
				 void *fb_data);

void
ice_renderer_output_set_framebuffer(struct weston_output *output,
				    void *fb_data);

int
ice_renderer_output_create(struct weston_output *output);

void
ice_renderer_output_destroy(struct weston_output *output);

int
ice_renderer_init(struct weston_compositor *ec);

#endif /* !ICE_RENDERER_H_ */
