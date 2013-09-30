#ifndef GDL_BUFFER_H_
# define GDL_BUFFER_H_

#include <gdl_types.h>

struct wl_gdl_buffer;
struct wl_gdl_buffer *wl_gdl_buffer_get(struct wl_resource *resource);
gdl_surface_info_t *
wl_gdl_buffer_get_surface_info(struct wl_gdl_buffer *buffer);

struct wl_gdl_sideband_buffer;
struct wl_gdl_sideband_buffer *
wl_gdl_sideband_buffer_get(struct wl_resource *resource);
uint32_t wl_gdl_sideband_buffer_get_width(struct wl_gdl_sideband_buffer *buffer);
uint32_t wl_gdl_sideband_buffer_get_height(struct wl_gdl_sideband_buffer *buffer);

#endif /* !GDL_BUFFER_H_ */
