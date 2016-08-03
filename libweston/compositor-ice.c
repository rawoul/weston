#define _GNU_SOURCE

#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <dirent.h>

#include <linux/input.h>

#include <libgdl.h>
#include <x86_cache.h>

#include "compositor.h"
#include "compositor-ice.h"
#include "lh-input.h"
#include "pixman-renderer.h"
#include "gdl-buffer.h"
#include "ice-renderer.h"

#include "presentation-time-server-protocol.h"
#include "gdl-server-protocol.h"
#include "shared/helpers.h"

//#define DEBUG

// output mode flag to filter out client-requested mode switch
#define ICE_OUTPUT_MODE_TVMODE 0x8000

// dummy buffer id when gdl buffers are flipped client-side
#define GDL_SURFACE_VIDEO ((gdl_surface_id_t)-2)
#define GDL_SURFACE_DUMMY ((gdl_surface_id_t)-3)

#define CURSOR_SIZE 32

enum ice_tint {
	ICE_TINT_NONE,
	ICE_TINT_RED,
	ICE_TINT_GREEN,
	ICE_TINT_BLUE,
};

enum ice_plane_mode {
	ICE_PLANE_GRAPHICS,
	ICE_PLANE_VIDEO,
	ICE_PLANE_BYPASS,
	ICE_PLANE_DISABLED,
};

enum ice_sideband_type {
	ICE_SIDEBAND_VIDEO,
	ICE_SIDEBAND_BYPASS,
};

struct ice_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;
	struct input_lh input;
	struct wl_event_source *gdl_event_source;
	int gdl_event_fd;
	int use_pixman;
	bool debug_planes;
	struct weston_layer background_layer;
	struct weston_surface *background_surface;
	struct weston_view *background_view;
};

struct ice_framebuffer {
	gdl_surface_info_t surface_info;
	pixman_image_t *image;
	void *renderer_state;
};

struct ice_plane_config {
	gdl_rectangle_t src_rect;
	gdl_rectangle_t dst_rect;
	gdl_pixel_format_t pixel_format;
	gdl_color_space_t color_space;
	gdl_boolean_t premul;
	gdl_boolean_t scale;
	gdl_uint8 alpha;
	enum ice_plane_mode mode;
	enum ice_tint tint;
};

struct ice_scanout_info {
	gdl_surface_id_t fb_id;
	struct weston_buffer_reference buffer_ref;
	int valid;
};

struct ice_plane {
	struct weston_plane base;
	gdl_plane_id_t id;
	gdl_plane_info_t caps;
	const char *name;
	struct ice_output *output;
	struct ice_plane_config config;
	struct ice_plane_config pending_config;
	struct ice_scanout_info scanout;
	struct ice_scanout_info pending_scanout;
	int vblank_delayed;
	int acquire_count;
};

struct ice_cursor {
	gdl_surface_info_t surface_info;
	gdl_uint8 *data;
	pixman_image_t *image;
	int x_offset;
	int y_offset;
};

struct ice_output {
	struct weston_output base;
	gdl_display_id_t disp_id;
	gdl_tvmode_t tvmode;
	gdl_plane_id_t scaled_plane;
	pixman_region32_t previous_damage;
	struct ice_framebuffer fb[2];
	struct ice_plane planes[4];
	struct ice_plane cursor_plane;
	struct ice_cursor cursor;
	gdl_upp_zorder_t pending_zorder;
	gdl_upp_zorder_t zorder;
	int num_planes;
	int current_fb;
	int vblank_pipe[2];
	int finish_frame;
	int flip_pending;
	struct timespec flip_ts;
	struct wl_event_source *vblank_source;
	pthread_t vblank_tid;
};

struct ice_mode {
	struct weston_mode base;
	gdl_boolean_t interlaced;
};

struct wl_gdl_buffer {
	struct wl_resource *resource;
	gdl_surface_info_t surface_info;
	gdl_color_space_t color_space;
};

struct wl_gdl_sideband_buffer {
	struct wl_resource *resource;
	struct ice_plane *plane;
	enum ice_sideband_type type;
	uint32_t width;
	uint32_t height;
};

static void ice_output_fini_vblank(struct ice_output *output);

#ifdef DEBUG
# define dbg(fmt, ...) weston_log(fmt, ##__VA_ARGS__)
#else
# define dbg(fmt, ...) ({})
#endif

static inline int timespec_cmp(const struct timespec *a,
			       const struct timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec - b->tv_sec;
	else
		return a->tv_nsec - b->tv_nsec;
}

static inline struct ice_mode *
ice_mode(struct weston_mode *base)
{
	return container_of(base, struct ice_mode, base);
}

static inline struct ice_output *
ice_output(struct weston_output *base)
{
	return container_of(base, struct ice_output, base);
}

static inline struct ice_backend *
ice_backend(struct weston_compositor *compositor)
{
	return container_of(compositor->backend, struct ice_backend, base);
}

static void
ice_restore(struct weston_compositor *base)
{
}

static void
ice_destroy(struct weston_compositor *ec)
{
	struct ice_backend *b = ice_backend(ec);

	if (b->gdl_event_source)
		wl_event_source_remove(b->gdl_event_source);

	if (b->gdl_event_fd != -1) {
		gdl_event_unregister(GDL_APP_EVENT_MODE_DISP_0);
		close(b->gdl_event_fd);
	}

	input_lh_shutdown(&b->input);
	weston_compositor_shutdown(ec);
	free(b);

	gdl_close();
}

static int
ice_fb_init(struct ice_output *output, struct ice_framebuffer *fb,
	    int width, int height)
{
	struct ice_backend *backend = ice_backend(output->base.compositor);
	gdl_ret_t rc;
	gdl_uint8 *map;

	/* use a fixed 1920x1080 fb size to avoid fragmentation */
	rc = gdl_alloc_surface(GDL_PF_ARGB_32,
			       MAX(width, 1920),
			       MAX(height, 1080),
			       0, &fb->surface_info);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to allocate %ux%u surface: %s\n",
			   width, height, gdl_get_error_string(rc));
		return -1;
	}

	fb->surface_info.width = width;
	fb->surface_info.height = height;

	rc = gdl_map_surface(fb->surface_info.id, &map, NULL);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to map surface: %s\n",
			   gdl_get_error_string(rc));
		goto err_map;
	}

	fb->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height,
					     (uint32_t *) map,
					     fb->surface_info.pitch);
	if (!fb->image)
		goto err_image;

	if (!backend->use_pixman) {
		fb->renderer_state = ice_renderer_create_framebuffer(
			backend->compositor->renderer, &fb->surface_info, map);
	} else
		fb->renderer_state = NULL;

	return 0;

err_image:
	gdl_unmap_surface(fb->surface_info.id);
err_map:
	gdl_free_surface(fb->surface_info.id);
	fb->surface_info.id = GDL_SURFACE_INVALID;
	return -1;
}

static void
ice_fb_cleanup(struct ice_output *output, struct ice_framebuffer *fb)
{
	struct ice_backend *backend = ice_backend(output->base.compositor);

	if (fb->renderer_state)
		ice_renderer_destroy_framebuffer(backend->compositor->renderer,
						 fb->renderer_state);

	if (fb->image) {
		pixman_image_unref(fb->image);
		fb->image = NULL;
	}

	if (fb->surface_info.id != GDL_SURFACE_INVALID) {
		gdl_ret_t rc;

		rc = gdl_unmap_surface(fb->surface_info.id);
		if (rc != GDL_SUCCESS)
			weston_log("failed to unmap fb surface %d: %s\n",
				   fb->surface_info.id,
				   gdl_get_error_string(rc));

		rc = gdl_free_surface(fb->surface_info.id);
		if (rc != GDL_SUCCESS)
			weston_log("failed to free fb surface %d: %s\n",
				   fb->surface_info.id,
				   gdl_get_error_string(rc));

		fb->surface_info.id = GDL_SURFACE_INVALID;
	}
}

static int
ice_output_is_interlaced(struct ice_output *output)
{
	return ice_mode(output->base.current_mode)->interlaced;
}

static int
gdl_pixel_format_has_alpha(gdl_pixel_format_t pixel_format)
{
	switch (pixel_format) {
	case GDL_PF_ARGB_32:
	case GDL_PF_ARGB_16_1555:
	case GDL_PF_ARGB_16_4444:
	case GDL_PF_ARGB_8:
	case GDL_PF_AYUV_8:
	case GDL_PF_AY16:
	case GDL_PF_ABGR_32:
	case GDL_PF_AYUV_32:
		return 1;
	default:
		return 0;
	}
}

static int
gdl_pixel_format_is_rgb(gdl_pixel_format_t pixel_format)
{
	switch (pixel_format) {
	case GDL_PF_ARGB_32:
	case GDL_PF_RGB_32:
	case GDL_PF_RGB_30:
	case GDL_PF_RGB_24:
	case GDL_PF_ARGB_16_1555:
	case GDL_PF_ARGB_16_4444:
	case GDL_PF_RGB_16:
	case GDL_PF_RGB_15:
	case GDL_PF_RGB_8:
	case GDL_PF_ARGB_8:
	case GDL_PF_A1:
	case GDL_PF_A4:
	case GDL_PF_A8:
	case GDL_PF_RGB_36:
	case GDL_PF_ABGR_32:
		return 1;
	default:
		return 0;
	}
}

static int
gdl_surface_get_color_space(gdl_surface_info_t *surface)
{
	if (gdl_pixel_format_is_rgb(surface->pixel_format))
		return GDL_COLOR_SPACE_RGB;
	else if (surface->width >= 720)
		return GDL_COLOR_SPACE_BT709;
	else
		return GDL_COLOR_SPACE_BT601;
}

static int
ice_plane_reconfigure(struct ice_plane *plane, struct ice_plane_config *cfg)
{
	struct ice_backend *backend =
		ice_backend(plane->output->base.compositor);
	gdl_boolean_t abort;
	gdl_ret_t rc;
	gdl_boolean_t vid_mute;
	gdl_boolean_t hide;

	abort = GDL_TRUE;

	rc = gdl_plane_config_begin(plane->id);
	if (rc != GDL_SUCCESS)
		goto commit;

	switch (cfg->mode) {
	case ICE_PLANE_DISABLED:
		dbg("hide plane %s\n", plane->name);
		vid_mute = GDL_TRUE;
		hide = GDL_TRUE;
		break;

	case ICE_PLANE_BYPASS:
		dbg("configure plane %s for bypass\n", plane->name);
		vid_mute = GDL_FALSE;
		hide = GDL_FALSE;
		break;

	case ICE_PLANE_VIDEO:
		dbg("configure video plane %s "
		    "a=%u src=%dx%d%+d%+d dst=%dx%d%+d%+d\n",
		    plane->name, cfg->alpha,
		    cfg->src_rect.width, cfg->src_rect.height,
		    cfg->src_rect.origin.x, cfg->src_rect.origin.y,
		    cfg->dst_rect.width, cfg->dst_rect.height,
		    cfg->dst_rect.origin.x, cfg->dst_rect.origin.y);

		gdl_plane_set_uint(GDL_PLANE_ALPHA_PREMULT, GDL_FALSE);
		gdl_plane_set_uint(GDL_PLANE_ALPHA_GLOBAL, cfg->alpha *
				   (backend->debug_planes ? 0.8 : 1));
		gdl_plane_set_uint(GDL_PLANE_VID_MISMATCH_POLICY,
				   GDL_VID_POLICY_CONSTRAIN);
		gdl_plane_set_rect(GDL_PLANE_VID_SRC_RECT, &cfg->src_rect);
		gdl_plane_set_rect(GDL_PLANE_VID_DST_RECT, &cfg->dst_rect);
		gdl_plane_set_uint(GDL_PLANE_VID_MUTE, GDL_FALSE);

		vid_mute = GDL_FALSE;
		hide = GDL_FALSE;
		break;

	case ICE_PLANE_GRAPHICS:
		dbg("configure graphics plane %s fmt=%s cs=%s a=%u "
		    "src=%dx%d%+d%+d dst=%dx%d%+d%+d\n",
		    plane->name, gdl_dbg_string_pixel_format(cfg->pixel_format),
		    gdl_dbg_string_color_space(cfg->color_space), cfg->alpha,
		    cfg->src_rect.width, cfg->src_rect.height,
		    cfg->src_rect.origin.x, cfg->src_rect.origin.y,
		    cfg->dst_rect.width, cfg->dst_rect.height,
		    cfg->dst_rect.origin.x, cfg->dst_rect.origin.y);

		gdl_plane_set_uint(GDL_PLANE_SRC_COLOR_SPACE, cfg->color_space);
		gdl_plane_set_uint(GDL_PLANE_PIXEL_FORMAT, cfg->pixel_format);
		gdl_plane_set_uint(GDL_PLANE_ALPHA_PREMULT, cfg->premul);
		// render graphic planes with a slight translucency to counter
		// Philips patent EP0838117B1
		gdl_plane_set_uint(GDL_PLANE_ALPHA_GLOBAL, cfg->alpha *
				   (backend->debug_planes ? 0.8 : 0.96));
		gdl_plane_set_rect(GDL_PLANE_SRC_RECT, &cfg->src_rect);
		gdl_plane_set_rect(GDL_PLANE_DST_RECT, &cfg->dst_rect);

		vid_mute = GDL_TRUE;
		hide = GDL_FALSE;
		break;

	default:
		assert("unhandled plane mode");
		return -1;
	}

	gdl_plane_set_uint(GDL_PLANE_HIDE, hide);

	if (plane->id != GDL_PLANE_ID_IAP_A &&
	    plane->id != GDL_PLANE_ID_IAP_B) {
		gdl_plane_set_uint(GDL_PLANE_VID_MUTE, vid_mute);
		gdl_plane_set_uint(GDL_PLANE_SCALE, cfg->scale && !hide);
	}

	if (plane->config.tint != cfg->tint) {
		gdl_csc_t csc = { 0 };
		int i;

		for (i = 0; i < 9; i++)
			csc.c[i] = 1;

		switch (cfg->tint) {
		case ICE_TINT_RED:
			csc.cr_ioff = 512;
			break;
		case ICE_TINT_BLUE:
			csc.cb_ioff = 512;
			break;
		case ICE_TINT_GREEN:
			csc.yg_ioff = 512;
			break;
		default:
			break;
		}

		gdl_plane_set_attr(GDL_PLANE_CSC_ADJUST, &csc);
	}

	abort = GDL_FALSE;

commit:
	rc = gdl_plane_config_end(abort);
	if (abort || rc != GDL_SUCCESS) {
		weston_log("failed to configure plane %s: %s\n", plane->name,
			   gdl_get_error_string(rc));
		return -1;
	}

	plane->config = *cfg;

	return 0;
}

static void
ice_plane_reset_config(struct ice_plane *plane)
{
	plane->config.src_rect.origin.x = 0;
	plane->config.src_rect.origin.y = 0;
	plane->config.src_rect.width = 0;
	plane->config.src_rect.height = 0;
	plane->config.dst_rect = plane->config.src_rect;
	plane->config.pixel_format = -1;
	plane->config.color_space = -1;
	plane->config.scale = 0;
	plane->config.premul = 0;
	plane->config.alpha = 255;
	plane->config.mode = -1;
}

static int
ice_plane_reset(struct ice_plane *plane)
{
	struct ice_output *output = plane->output;
	gdl_plane_id_t id;
	gdl_ret_t rc;

	dbg("reset plane %s\n", plane->name);

	ice_plane_reset_config(plane);

	// reset plane
	rc = gdl_plane_reset(plane->id);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to reset plane %s: %s\n",
			   plane->name, gdl_get_error_string(rc));
		return -1;
	}

	if (output->scaled_plane == plane->id) {
		// disable scaling on other planes to avoid conflict
		for (id = GDL_PLANE_ID_UPP_A; id <= GDL_PLANE_ID_UPP_E; id++) {
			if (plane->id == id)
				continue;

			rc = gdl_plane_config_begin(id);
			if (rc != GDL_SUCCESS)
				continue;

			gdl_plane_set_uint(GDL_PLANE_SCALE, GDL_FALSE);

			rc = gdl_plane_config_end(GDL_FALSE);
			if (rc != GDL_SUCCESS)
				weston_log("failed to disable scaling on "
					   "plane %s: %s\n", plane->name,
					   gdl_get_error_string(rc));
		}
	}

	return 0;
}

static bool
gdl_surface_is_dummy(gdl_surface_id_t id)
{
	return id == GDL_SURFACE_VIDEO || id == GDL_SURFACE_DUMMY;
}

static bool
ice_plane_config_compatible(const struct ice_plane_config *prev_config,
			    const struct ice_plane_config *new_config)
{
	if (prev_config->mode != ICE_PLANE_GRAPHICS ||
	    new_config->mode != ICE_PLANE_GRAPHICS)
		return true;

	if (prev_config->mode != new_config->mode)
		return false;

	if (prev_config->mode != new_config->mode)
		return false;

	if (prev_config->pixel_format != new_config->pixel_format)
		return false;

	if (memcmp(&prev_config->src_rect, &new_config->src_rect,
		   sizeof (gdl_rectangle_t)))
		return false;

	if (memcmp(&prev_config->dst_rect, &new_config->dst_rect,
		   sizeof (gdl_rectangle_t)))
		return false;

	return true;
}

static int
ice_plane_commit_flip(struct ice_plane *plane)
{
	struct ice_scanout_info *s;
	struct ice_plane_config *cfg;
	gdl_ret_t rc;
	int reset;

	reset = 0;

	s = &plane->pending_scanout;
	if (!s->valid)
		return 0;

	cfg = &plane->pending_config;

	dbg("flip fb=%02d on plane %s\n", s->fb_id, plane->name);

again:
	if (memcmp(&plane->config, cfg, sizeof (*cfg)) != 0) {
		if (!ice_plane_config_compatible(&plane->config, cfg)) {
			dbg("clear flip before reconfigure\n");
			gdl_flip(plane->id, GDL_SURFACE_INVALID,
				 GDL_FLIP_ASYNC);
		}

		if (ice_plane_reconfigure(plane, &plane->pending_config))
			goto fail;
	}

	if (!gdl_surface_is_dummy(s->fb_id)) {
		rc = gdl_flip(plane->id, s->fb_id, GDL_FLIP_ASYNC);
		if (rc != GDL_SUCCESS) {
			weston_log("failed to flip surface %u: %s\n",
				   s->fb_id, gdl_get_error_string(rc));
			goto fail;
		}
	}

	return 0;

fail:
	if (!reset) {
		ice_plane_reset(plane);
		reset = 1;
		goto again;
	}

	weston_buffer_reference(&s->buffer_ref, NULL);
	s->valid = 0;
	s->fb_id = GDL_SURFACE_INVALID;

	return -1;
}

static int
ice_plane_prepare_scanout(struct ice_plane *plane,
			  struct ice_plane_config *cfg,
			  gdl_surface_id_t scanout_fb,
			  struct weston_buffer *buffer)
{
	struct ice_output *output = plane->output;

	assert(!plane->pending_scanout.valid);

	plane->vblank_delayed = 0;
	plane->pending_scanout.valid = 1;
	plane->pending_scanout.fb_id = scanout_fb;

	weston_buffer_reference(&plane->pending_scanout.buffer_ref, buffer);

	if (cfg) {
		plane->pending_config = *cfg;
		if (cfg->scale)
			output->scaled_plane = plane->id;
	}

	switch (scanout_fb) {
	case GDL_SURFACE_INVALID:
		plane->pending_config.mode = ICE_PLANE_DISABLED;
		break;
	case GDL_SURFACE_VIDEO:
		plane->pending_config.mode = ICE_PLANE_VIDEO;
		break;
	case GDL_SURFACE_DUMMY:
		plane->pending_config.mode = ICE_PLANE_BYPASS;
		break;
	default:
		plane->pending_config.mode = ICE_PLANE_GRAPHICS;
		break;
	}

	return 0;
}

static void
ice_plane_stack(struct ice_plane *plane)
{
	gdl_upp_zorder_t *zorder = &plane->output->pending_zorder;

	if (plane->id >= GDL_PLANE_ID_UPP_A &&
	    plane->id <= GDL_PLANE_ID_UPP_E)
		zorder->order[zorder->num_planes++] = plane->id;
}

static int
ice_plane_assign_fb(struct ice_plane *plane, struct ice_framebuffer *fb)
{
	struct ice_backend *backend =
		ice_backend(plane->output->base.compositor);
	struct ice_plane_config cfg;

	memset(&cfg, 0, sizeof cfg);

	cfg.alpha = 255;
	cfg.src_rect.width = fb->surface_info.width;
	cfg.src_rect.height = fb->surface_info.height;
	cfg.dst_rect = cfg.src_rect;
	cfg.pixel_format = fb->surface_info.pixel_format;
	cfg.color_space = gdl_surface_get_color_space(&fb->surface_info);
	cfg.premul = gdl_pixel_format_has_alpha(cfg.pixel_format);

	if (backend->debug_planes)
		cfg.tint = ICE_TINT_RED;

	if (ice_plane_prepare_scanout(plane, &cfg, fb->surface_info.id, NULL))
		return -1;

	dbg("assigned framebuffer fb=%02u to plane %s\n",
	    fb->surface_info.id, plane->name);

	ice_plane_stack(plane);

	return 0;
}

static int
ice_plane_assign_video(struct ice_plane *plane, struct weston_view *view)
{
	struct ice_output *output = plane->output;
	struct ice_backend *backend = ice_backend(output->base.compositor);
	struct ice_plane_config cfg;
	struct weston_surface *surface;
	struct weston_buffer *buffer;
	pixman_box32_t *surf_extents, *clip_extents;
	pixman_region32_t clip;

	if (plane->acquire_count > 0)
		return -1;

	surface = view->surface;

	buffer = surface->buffer_ref.buffer;
	if (!buffer)
		return -1;

	dbg("try to assign plane %s for %ux%u video buffer\n", plane->name,
	    buffer->width, buffer->height);

	surf_extents = pixman_region32_extents(&view->transform.boundingbox);

	pixman_region32_init(&clip);
	pixman_region32_intersect(&clip, &plane->output->base.region,
				  &view->transform.boundingbox);

	clip_extents = pixman_region32_extents(&clip);

	memset(&cfg, 0, sizeof cfg);
	cfg.dst_rect.origin.x = clip_extents->x1 - output->base.x;
	cfg.dst_rect.origin.y = clip_extents->y1 - output->base.y;
	cfg.dst_rect.width = clip_extents->x2 - clip_extents->x1;
	cfg.dst_rect.height = clip_extents->y2 - clip_extents->y1;

	pixman_region32_fini(&clip);

	if (!cfg.dst_rect.width || !cfg.dst_rect.height) {
		dbg(" -> skip dr, surface out of screen\n");
		return -1;
	}

	cfg.src_rect = cfg.dst_rect;
	cfg.src_rect.origin.x -= surf_extents->x1;
	cfg.src_rect.origin.y -= surf_extents->y1;

	// make sure dest line and height are even when output is interlaced
	if (ice_output_is_interlaced(output)) {
		if (cfg.dst_rect.origin.y & 1)
			cfg.dst_rect.origin.y--;
		if (cfg.dst_rect.height & 1)
			cfg.dst_rect.height--;
	}

	dbg(" . dst rect %dx%d%+d%+d\n",
	    cfg.dst_rect.width, cfg.dst_rect.height,
	    cfg.dst_rect.origin.x, cfg.dst_rect.origin.y);

	dbg(" . src rect %dx%d%+d%+d\n",
	    cfg.src_rect.width, cfg.src_rect.height,
	    cfg.src_rect.origin.x, cfg.src_rect.origin.y);

	cfg.alpha = roundf(weston_view_get_alpha(view) * 255.0f);

	if (backend->debug_planes)
		cfg.tint = ICE_TINT_GREEN;

	if (ice_plane_prepare_scanout(plane, &cfg, GDL_SURFACE_VIDEO, buffer))
		return -1;

	dbg("assigned video buffer to plane %s\n", plane->name);

	ice_plane_stack(plane);

	return 0;
}

static int
ice_plane_assign_dummy(struct ice_plane *plane, struct weston_view *view)
{
	struct ice_output *output = plane->output;
	struct ice_backend *backend = ice_backend(output->base.compositor);
	struct ice_plane_config cfg;
	struct weston_buffer *buffer;

	if (plane->acquire_count == 0)
		return -1;

	buffer = view->surface->buffer_ref.buffer;
	if (!buffer)
		return -1;

	dbg("try to assign plane %s for %ux%u dummy buffer\n", plane->name,
	    buffer->width, buffer->height);

	memset(&cfg, 0, sizeof cfg);
	if (backend->debug_planes)
		cfg.tint = ICE_TINT_GREEN;

	if (ice_plane_prepare_scanout(plane, &cfg, GDL_SURFACE_DUMMY, buffer))
		return -1;

	dbg("assigned dummy buffer to plane %s\n", plane->name);

	ice_plane_stack(plane);

	return 0;
}

static int
ice_plane_assign_graphics(struct ice_plane *plane, struct weston_view *view)
{
	struct ice_output *output = plane->output;
	struct ice_backend *backend = ice_backend(output->base.compositor);
	struct ice_plane_config cfg;
	struct weston_surface *surface;
	struct weston_buffer *buffer;
	struct weston_buffer_viewport *vp;
	struct wl_gdl_buffer *gdl_buffer;
	gdl_surface_info_t *surface_info;
	float sx1, sy1, sx2, sy2;

	surface = view->surface;

	buffer = surface->buffer_ref.buffer;
	if (!buffer)
		return -1;

	if (!(gdl_buffer = wl_gdl_buffer_get(buffer->resource)))
		return -1;

	vp = &surface->buffer_viewport;
	surface_info = wl_gdl_buffer_get_surface_info(gdl_buffer);

	dbg("try to assign %s plane for %ux%u %s surface\n", plane->name,
	    surface_info->width, surface_info->height,
	    gdl_dbg_string_pixel_format(surface_info->pixel_format));

	// check buffer pixel format is supported on the plane
	if (!plane->caps.pixel_formats[surface_info->pixel_format]) {
		dbg( " -> skip dr, unsupported pixel format\n");
		return -1;
	}

	// check there is no rotation / flip transformation
	if (output->base.transform != WL_OUTPUT_TRANSFORM_NORMAL ||
	    vp->buffer.transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		dbg(" -> skip dr, output or buffer is transformed\n");
		return -1;
	}

	// check view transformation is limited to translation and scaling
	if (view->transform.enabled) {
		int allowed_transform = WESTON_MATRIX_TRANSFORM_TRANSLATE;

		if (output->scaled_plane == GDL_PLANE_ID_UNDEFINED)
			allowed_transform |= WESTON_MATRIX_TRANSFORM_SCALE;

		if ((view->transform.matrix.type & ~allowed_transform) != 0) {
			dbg(" -> skip dr, unsupported view transform\n");
			return -1;
		}
	}

	struct weston_vector p1 =
		{{ 0.0f, 0.0f, 0.0f, 1.0f }};
	struct weston_vector p2 =
		{{ surface->width, surface->height, 0.0f, 1.0f }};

	{
		struct weston_matrix matrix = view->transform.matrix;
		weston_matrix_multiply(&matrix, &output->base.matrix);
		weston_matrix_transform(&matrix, &p1);
		weston_matrix_transform(&matrix, &p2);
	}

	dbg(" . dst rect %gx%g%+g%+g\n",
	    p2.f[0] - p1.f[0], p2.f[1] - p1.f[1], p1.f[0], p1.f[1]);

	sx1 = 0;
	sy1 = 0;
	sx2 = surface->width;
	sy2 = surface->height;

	// clip top-left corner
	if (p1.f[0] >= output->base.width)
		goto offscreen;

	if (p1.f[1] >= output->base.height)
		goto offscreen;

	if (p1.f[0] < 0) {
		sx1 -= p1.f[0];
		p1.f[0] = 0;
	}

	if (p1.f[1] < 0) {
		sy1 -= p1.f[1];
		p1.f[1] = 0;
	}

	// clip bottom-right corner
	if (p2.f[0] <= 0)
		goto offscreen;

	if (p2.f[1] <= 0)
		goto offscreen;

	if (p2.f[0] > output->base.width) {
		sx2 -= p2.f[0] - output->base.width;
		p2.f[0] = output->base.width;
	}

	if (p2.f[1] > output->base.height) {
		sy2 -= p2.f[1] - output->base.height;
		p2.f[1] = output->base.height;
	}

	// skip clipped out view
	if (p2.f[0] - p1.f[0] <= 0 || p2.f[1] - p1.f[1] <= 0)
		goto offscreen;

	// round clipped coordinates
	memset(&cfg, 0, sizeof cfg);
	cfg.dst_rect.origin.x = nearbyintf(p1.f[0]);
	cfg.dst_rect.origin.y = nearbyintf(p1.f[1]);
	cfg.dst_rect.width = nearbyintf(p2.f[0] - p1.f[0]);
	cfg.dst_rect.height = nearbyintf(p2.f[1] - p1.f[1]);

	// make sure dest line and height are even when output is interlaced
	if (ice_output_is_interlaced(plane->output)) {
		if (cfg.dst_rect.origin.y & 1)
			cfg.dst_rect.origin.y--;
		if (cfg.dst_rect.height & 1) {
			cfg.dst_rect.height--;
			sy2--;
		}
	}

	dbg(" . clipped to %dx%d%+d%+d\n",
	    cfg.dst_rect.width, cfg.dst_rect.height,
	    cfg.dst_rect.origin.x, cfg.dst_rect.origin.y);

	// check clipped size is supported
	if (cfg.dst_rect.width < plane->caps.min_dst_rect.width ||
	    cfg.dst_rect.height < plane->caps.min_dst_rect.height ||
	    cfg.dst_rect.width > plane->caps.max_dst_rect.width ||
	    cfg.dst_rect.height > plane->caps.max_dst_rect.height) {
		dbg(" -> skip dr, surface size not supported by hw\n");
		return -1;
	}

	// check opaque region when using a pixel format with alpha
	cfg.pixel_format = surface_info->pixel_format;

	if (gdl_pixel_format_has_alpha(cfg.pixel_format) &&
	    pixman_region32_not_empty(&surface->opaque)) {
		pixman_region32_t non_opaque;

		if (cfg.pixel_format != GDL_PF_ARGB_32) {
			dbg(" -> skip dr, opaque surface with alpha format\n");
			return -1;
		}

		pixman_region32_init_rect(&non_opaque, 0, 0,
					  surface->width, surface->height);
		pixman_region32_subtract(&non_opaque, &non_opaque,
					 &surface->opaque);

		if (pixman_region32_not_empty(&non_opaque)) {
			pixman_region32_fini(&non_opaque);
			dbg(" -> skip dr, argb surface is not totally opaque\n");
			return -1;
		}

		pixman_region32_fini(&non_opaque);
		cfg.pixel_format = GDL_PF_RGB_32;
	}

	// convert output coordinates back to buffer coordinates
	weston_surface_to_buffer_float(surface, sx1, sy1, &sx1, &sy1);
	weston_surface_to_buffer_float(surface, sx2, sy2, &sx2, &sy2);

	cfg.src_rect.origin.x = nearbyintf(sx1);
	cfg.src_rect.origin.y = nearbyintf(sy1);
	cfg.src_rect.width = nearbyintf(sx2 - sx1);
	cfg.src_rect.height = nearbyintf(sy2 - sy1);

	if (ice_output_is_interlaced(plane->output)) {
		if (cfg.src_rect.origin.y & 1)
			cfg.src_rect.origin.y--;
		if (cfg.src_rect.height & 1)
			cfg.src_rect.height--;
	}

	dbg(" . src rect: %dx%d%+d%+d\n",
	    cfg.src_rect.width, cfg.src_rect.height,
	    cfg.src_rect.origin.x, cfg.src_rect.origin.y);

	// check scaler configuration
	if (cfg.src_rect.width != cfg.dst_rect.width ||
	    cfg.src_rect.height != cfg.dst_rect.height) {
		// we can only scale one plane on CE4100
		if (output->scaled_plane != GDL_PLANE_ID_UNDEFINED) {
			dbg(" -> skip dr, no scaler left\n");
			return -1;
		}

		// cannot downscale on CE4100
		if (cfg.src_rect.width > cfg.dst_rect.width ||
		    cfg.src_rect.height > cfg.dst_rect.height) {
			dbg(" -> skip dr, cannot downscale\n");
			return -1;
		}

		// cannot scale large buffer on CE4100
		if (cfg.src_rect.width > 1280) {
			dbg(" -> skip dr, cannot scale >1280 src width\n");
			return -1;
		}

		// scaler output is ugly in interlaced mode, avoid it
		if (ice_output_is_interlaced(plane->output)) {
			dbg(" -> skip dr, avoid scaling in interlaced mode\n");
			return -1;
		}

		cfg.scale = 1;
	}

	cfg.alpha = roundf(weston_view_get_alpha(view) * 255.0f);
	cfg.color_space = gdl_buffer->color_space;
	cfg.premul = gdl_pixel_format_has_alpha(cfg.pixel_format);

	if (backend->debug_planes)
		cfg.tint = ICE_TINT_BLUE;

	if (ice_plane_prepare_scanout(plane, &cfg, surface_info->id, buffer)) {
		dbg(" -> skip dr, flip failed\n");
		return -1;
	}

	dbg("assigned surface fb=%02u to plane %s\n",
	    surface_info->id, plane->name);

	ice_plane_stack(plane);

	return 0;

offscreen:
	dbg(" -> skip dr, surface out of screen\n");
	return -1;
}

static int
ice_cursor_init(struct ice_cursor *cursor, int width, int height)
{
	gdl_ret_t rc;
	gdl_palette_t palette;
	int i;

	rc = gdl_alloc_surface(GDL_PF_ARGB_8, width, height,
			       GDL_SURFACE_CACHED,
			       &cursor->surface_info);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to allocate %ux%u surface: %s\n",
			   width, height, gdl_get_error_string(rc));
		return -1;
	}

	rc = gdl_map_surface(cursor->surface_info.id, &cursor->data, NULL);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to map surface: %s\n",
			   gdl_get_error_string(rc));
		goto err_map;
	}

	memset(cursor->data, 0, cursor->surface_info.pitch * height);

	/* simple palette that maps to pixman a2r2g2b2 format */
	palette.length = 256;
	for (i = 0; i < 256; i++) {
		palette.data[i].a = ((i & 0xc0) >> 6) * 0x55;
		palette.data[i].r_y = ((i & 0x30) >> 4) * 0x55;
		palette.data[i].g_u = ((i & 0x0c) >> 2) * 0x55;
		palette.data[i].b_v = (i & 0x03) * 0x55;
	}

	gdl_set_palette(cursor->surface_info.id, &palette);

	cursor->image = pixman_image_create_bits(PIXMAN_a2r2g2b2, width, height,
						 (uint32_t *) cursor->data,
						 cursor->surface_info.pitch);
	if (!cursor->image) {
		weston_log("failed to create cursor image\n");
		goto err_image;
	}

	return 0;

err_image:
	gdl_unmap_surface(cursor->surface_info.id);
err_map:
	gdl_free_surface(cursor->surface_info.id);
	cursor->surface_info.id = GDL_SURFACE_INVALID;
	return -1;
}

static void
ice_cursor_cleanup(struct ice_cursor *cursor)
{
	if (cursor->image) {
		pixman_image_unref(cursor->image);
		cursor->image = NULL;
	}

	if (cursor->surface_info.id != GDL_SURFACE_INVALID) {
		gdl_unmap_surface(cursor->surface_info.id);
		gdl_free_surface(cursor->surface_info.id);
		cursor->surface_info.id = GDL_SURFACE_INVALID;
	}

	cursor->data = NULL;
}

static int
ice_cursor_set_buffer(struct ice_cursor *cursor, struct weston_buffer *buffer)
{
	struct wl_shm_buffer *shm_buffer;
	pixman_image_t *image;
	pixman_format_code_t format;
	int width;
	int height;
	int stride;
	void *data;

	if ((shm_buffer = wl_shm_buffer_get(buffer->resource))) {
		switch (wl_shm_buffer_get_format(shm_buffer)) {
		case WL_SHM_FORMAT_XRGB8888:
			format = PIXMAN_x8r8g8b8;
			break;
		case WL_SHM_FORMAT_ARGB8888:
			format = PIXMAN_a8r8g8b8;
			break;
		case WL_SHM_FORMAT_RGB565:
			format = PIXMAN_r5g6b5;
			break;
		default:
			dbg("unsupported format for cursor\n");
			return -1;
		}

		width = wl_shm_buffer_get_width(shm_buffer);
		height = wl_shm_buffer_get_height(shm_buffer);
		stride = wl_shm_buffer_get_stride(shm_buffer);
		data = wl_shm_buffer_get_data(shm_buffer);

	} else {
		dbg("unsupported buffer type for cursor\n");
		return -1;
	}

	image = pixman_image_create_bits(format, width, height, data, stride);
	if (!image) {
		weston_log("failed to create cursor image\n");
		return -1;
	}

	dbg("update cursor data\n");

	pixman_image_composite(PIXMAN_OP_SRC, image, NULL, cursor->image,
			       0, 0, 0, 0, cursor->x_offset, cursor->y_offset,
			       CURSOR_SIZE, CURSOR_SIZE);

	pixman_image_unref(image);

	cache_flush_buffer(cursor->data, cursor->surface_info.pitch *
			   cursor->surface_info.height);

	return 0;
}

static int
ice_plane_assign_cursor(struct ice_plane *plane,
			struct ice_cursor *cursor,
			struct weston_view *view)
{
	struct ice_output *output = plane->output;
	struct ice_plane_config cfg;
	gdl_surface_info_t *surface_info;
	pixman_region32_t cursor_region;
	pixman_region32_t clip;
	pixman_box32_t *surf_extents, *clip_extents;

	if (output->base.matrix.type & ~WESTON_MATRIX_TRANSFORM_TRANSLATE)
		return -1;

	surface_info = &cursor->surface_info;
	surf_extents = pixman_region32_extents(&view->transform.boundingbox);

	pixman_region32_init_rect(&cursor_region,
				  surf_extents->x1 - cursor->x_offset,
				  surf_extents->y1 - cursor->y_offset,
				  surface_info->width, surface_info->height);

	pixman_region32_init(&clip);
	pixman_region32_intersect(&clip, &plane->output->base.region,
				  &cursor_region);

	pixman_region32_fini(&cursor_region);

	if (!pixman_region32_not_empty(&clip)) {
		pixman_region32_fini(&clip);
		return -1;
	}

	memset(&cfg, 0, sizeof cfg);

	clip_extents = pixman_region32_extents(&clip);
	cfg.dst_rect.origin.x = clip_extents->x1 - output->base.x;
	cfg.dst_rect.origin.y = clip_extents->y1 - output->base.y;
	cfg.dst_rect.width = clip_extents->x2 - clip_extents->x1;
	cfg.dst_rect.height = clip_extents->y2 - clip_extents->y1;

	pixman_region32_fini(&clip);

	cfg.src_rect = cfg.dst_rect;
	cfg.src_rect.origin.x -= surf_extents->x1;
	cfg.src_rect.origin.y -= surf_extents->y1;
	cfg.src_rect.origin.x += cursor->x_offset;
	cfg.src_rect.origin.y += cursor->y_offset;

	// make sure dest line and height are even when output is interlaced
	if (ice_output_is_interlaced(output)) {
		if (cfg.dst_rect.origin.y & 1)
			cfg.dst_rect.origin.y--;
		if (cfg.dst_rect.height & 1) {
			cfg.dst_rect.height--;
			cfg.src_rect.height--;
		}
	}

	cfg.alpha = roundf(weston_view_get_alpha(view) * 255.0f);

	cfg.pixel_format = surface_info->pixel_format;
	cfg.color_space = gdl_surface_get_color_space(surface_info);
	cfg.premul = gdl_pixel_format_has_alpha(cfg.pixel_format);

	if (ice_plane_prepare_scanout(plane, &cfg, surface_info->id, NULL))
		return -1;

	dbg("assigned cursor fb=%02u to plane %s\n",
	    surface_info->id, plane->name);

	ice_plane_stack(plane);

	return 0;
}

static void
ice_plane_disable(struct ice_plane *plane)
{
	if (plane->scanout.valid && plane->scanout.fb_id == GDL_SURFACE_INVALID)
		return;

	ice_plane_prepare_scanout(plane, NULL, GDL_SURFACE_INVALID, NULL);
}

static int
ice_plane_finish_flip(struct ice_plane *plane)
{
	struct ice_scanout_info *scanout = &plane->scanout;
	struct ice_scanout_info *pending_scanout = &plane->pending_scanout;
	gdl_surface_id_t fb_id;
	gdl_ret_t rc;

	if (!pending_scanout->valid)
		return 0;

	if (pending_scanout->fb_id == GDL_SURFACE_VIDEO ||
	    pending_scanout->fb_id == GDL_SURFACE_DUMMY)
		fb_id = pending_scanout->fb_id;
	else {
		rc = gdl_plane_get_int(plane->id, GDL_PLANE_DISPLAYED_SURFACE,
				       (gdl_int32 *) &fb_id);
		if (rc != GDL_SUCCESS) {
			weston_log("failed to get plane %s display surface: %s\n",
				   plane->name, gdl_get_error_string(rc));
			fb_id = GDL_SURFACE_INVALID;
		}
	}

	if (!scanout->valid || scanout->fb_id != fb_id) {
		weston_buffer_reference(&scanout->buffer_ref, NULL);
		scanout->fb_id = fb_id;
		scanout->valid = 1;
	}

	if (pending_scanout->fb_id != fb_id) {
		if (!plane->vblank_delayed) {
			// flip did not complete yet, wait for the next vblank
			plane->vblank_delayed = 1;
			dbg("display of fb=%02d on plane %s delayed\n",
			    pending_scanout->fb_id, plane->name);
			return -1;
		} else {
			// we've seen two vblanks and the surface is still not
			// flipped, it's probably been destroyed since we might
			// not have ref'ed it (which can only be done by mapping)
			dbg("display of fb=%02d on plane %s aborted\n",
			    pending_scanout->fb_id, plane->name);
		}
	} else {
		// flip completed, keep reference on buffer
		weston_buffer_reference(&scanout->buffer_ref,
					pending_scanout->buffer_ref.buffer);
	}

	weston_buffer_reference(&pending_scanout->buffer_ref, NULL);
	pending_scanout->fb_id = GDL_SURFACE_INVALID;
	pending_scanout->valid = 0;

	dbg("displayed fb=%02d on plane %s\n", scanout->fb_id, plane->name);

	return 0;
}

static void
ice_plane_init(struct ice_plane *plane, gdl_plane_id_t id,
	       struct ice_output *output)
{
	weston_plane_init(&plane->base, output->base.compositor, 0, 0);

	plane->id = id;
	plane->name = gdl_dbg_string_plane_id(id);
	plane->output = output;

	plane->scanout.fb_id = GDL_SURFACE_INVALID;
	plane->scanout.valid = 0;

	plane->pending_scanout.fb_id = GDL_SURFACE_INVALID;
	plane->pending_scanout.valid = 0;

	ice_plane_reset(plane);

	if (gdl_plane_capabilities(id, &plane->caps) != GDL_SUCCESS) {
		weston_log("failed to get caps of plane %s", plane->name);
		memset(&plane->caps, 0, sizeof (plane->caps));
	}
}

static struct ice_plane *
ice_output_find_plane(struct ice_output *iceout, gdl_plane_id_t plane_id)
{
	int i;

	for (i = 0; i < iceout->num_planes; i++)
		if (iceout->planes[i].id == plane_id)
			return &iceout->planes[i];

	return NULL;
}

static int
ice_output_init(struct ice_output *output)
{
	struct ice_backend *backend = ice_backend(output->base.compositor);
	int w = output->base.current_mode->width;
	int h = output->base.current_mode->height;
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(output->fb); i++) {
		output->fb[i].surface_info.id = GDL_SURFACE_INVALID;
		output->fb[i].image = NULL;
	}

	for (i = 0; i < ARRAY_LENGTH(output->fb); i++)
		if (ice_fb_init(output, &output->fb[i], w, h) < 0)
			goto err;

	if (backend->use_pixman) {
		if (pixman_renderer_output_create(&output->base) < 0)
			goto err;
	} else {
		if (ice_renderer_output_create(&output->base) < 0)
			goto err;
	}

	pixman_region32_init_rect(&output->previous_damage,
				  output->base.x, output->base.y, w, h);

	return 0;

err:
	for (i = 0; i < ARRAY_LENGTH(output->fb); i++)
		ice_fb_cleanup(output, &output->fb[i]);

	return -1;
}

static void
ice_output_fini(struct ice_output *output)
{
	struct ice_backend *backend = ice_backend(output->base.compositor);
	unsigned int i;

	pixman_region32_fini(&output->previous_damage);

	for (i = 0; i < ARRAY_LENGTH(output->fb); i++)
		ice_fb_cleanup(output, &output->fb[i]);

	ice_cursor_cleanup(&output->cursor);

	if (backend->use_pixman) {
		pixman_renderer_output_destroy(&output->base);
		pixman_region32_fini(&output->previous_damage);
	} else
		ice_renderer_output_destroy(&output->base);
}

static int
ice_output_render(struct ice_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *ec = output->base.compositor;
	struct ice_backend *backend = ice_backend(ec);
	struct ice_plane *plane;
	int i;

	if (pixman_region32_not_empty(damage)) {
		struct ice_framebuffer *fb;
		pixman_region32_t total_damage;

		pixman_region32_init(&total_damage);
		pixman_region32_union(&total_damage, damage,
				      &output->previous_damage);
		pixman_region32_copy(&output->previous_damage, damage);

		output->current_fb ^= 1;
		fb = &output->fb[output->current_fb];

		if (backend->use_pixman)
			pixman_renderer_output_set_buffer(&output->base,
							  fb->image);
		else
			ice_renderer_output_set_framebuffer(&output->base,
							    fb->renderer_state);

		dbg("render output\n");
		ec->renderer->repaint_output(&output->base, &total_damage);

		pixman_region32_fini(&total_damage);

		pixman_region32_subtract(&ec->primary_plane.damage,
					 &ec->primary_plane.damage,
					 damage);
	}

	for (plane = NULL, i = 0; i < output->num_planes; i++) {
		struct ice_plane *p = &output->planes[i];
		if (!p->pending_scanout.valid && !p->acquire_count)
			plane = p;
	}

	if (!plane) {
		// No plane is available: this can happen if all views could
		// be used as scanout or if all planes were reserved by clients
		return 0;
	}

	return ice_plane_assign_fb(plane, &output->fb[output->current_fb]);
}

static int
ice_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct ice_output *output = ice_output(base);
	struct ice_plane *plane;
	gdl_ret_t rc;
	int ret, i;

	// render composited framebuffer if needed and assign it to a plane
	ret = ice_output_render(output, damage);

	// stack planes in the correct order
	if (memcmp(&output->zorder, &output->pending_zorder,
		   sizeof (gdl_upp_zorder_t)) != 0) {
		rc = gdl_set_upp_zorder(&output->pending_zorder);
		if (rc != GDL_SUCCESS)
			weston_log("failed to set upp zorder: %s\n",
				   gdl_get_error_string(rc));
		else
			output->zorder = output->pending_zorder;
	}

	memset(&output->pending_zorder, 0, sizeof (gdl_upp_zorder_t));

	// commit planes configuration and scanout buffer
	assert(!output->flip_pending);

	for (i = 0; i < output->num_planes; i++) {
		plane = &output->planes[i];
		if (!plane->pending_scanout.valid)
			ice_plane_disable(plane);

		ret |= ice_plane_commit_flip(plane);
	}

	weston_compositor_read_presentation_clock(base->compositor,
						  &output->flip_ts);

	plane = &output->cursor_plane;
	if (!plane->pending_scanout.valid)
		ice_plane_disable(plane);

	ret |= ice_plane_commit_flip(plane);

	output->flip_pending = 1;
	output->scaled_plane = GDL_PLANE_ID_UNDEFINED;

	return ret;
}

static void
ice_output_start_repaint_loop(struct weston_output *base)
{
	struct ice_output *output = ice_output(base);

	output->finish_frame = 1;
}

static struct weston_plane *
ice_output_assign_cursor_view(struct ice_output *output,
			      struct weston_view *view)
{
	struct ice_plane *plane;
	struct ice_cursor *cursor;
	int need_update;

	if (!view->surface->buffer_ref.buffer)
		return NULL;

	if (view->surface->width > CURSOR_SIZE ||
	    view->surface->height > CURSOR_SIZE)
		return NULL;

	plane = &output->cursor_plane;
	if (plane->pending_scanout.valid)
		return NULL;

	cursor = &output->cursor;
	if (!cursor->image) {
		int width, height;

		cursor->x_offset = plane->caps.min_dst_rect.width;
		cursor->y_offset = plane->caps.min_dst_rect.height;

		// make sure cursor surface can always be flipped
		width = CURSOR_SIZE + cursor->x_offset;
		height = CURSOR_SIZE + cursor->y_offset;

		if (ice_cursor_init(cursor, width, height) < 0)
			return NULL;
	}

	need_update = !plane->scanout.valid ||
		pixman_region32_not_empty(&view->surface->damage);

	if (need_update &&
	    ice_cursor_set_buffer(cursor, view->surface->buffer_ref.buffer))
		return NULL;

	view->surface->keep_buffer = 1;

	if (ice_plane_assign_cursor(plane, cursor, view))
		return NULL;

	return &plane->base;
}

static struct weston_plane *
ice_output_assign_sideband_view(struct ice_output *output,
				struct weston_view *view)
{
	struct ice_plane *plane;
	struct weston_buffer *buffer;
	struct wl_gdl_sideband_buffer *sideband_buffer;
	int ret;

	buffer = view->surface->buffer_ref.buffer;
	if (!buffer)
		return NULL;

	sideband_buffer = wl_gdl_sideband_buffer_get(buffer->resource);
	if (!sideband_buffer)
		return NULL;

	plane = sideband_buffer->plane;
	if (!plane || plane->pending_scanout.valid)
		return NULL;

	switch (sideband_buffer->type) {
	case ICE_SIDEBAND_VIDEO:
		ret = ice_plane_assign_video(plane, view);
		break;
	case ICE_SIDEBAND_BYPASS:
		ret = ice_plane_assign_dummy(plane, view);
		break;
	default:
		ret = -1;
		break;
	}

	if (ret)
		return NULL;

	return &plane->base;
}

static struct weston_plane *
ice_output_assign_graphics_view(struct ice_output *output,
				struct weston_view *view)
{
	struct weston_compositor *ec = output->base.compositor;
	struct ice_plane *plane;
	int free_planes;
	int i;

	plane = NULL;
	free_planes = 0;

	for (i = 0; i < output->num_planes; i++) {
		struct ice_plane *p = &output->planes[i];
		if (!p->pending_scanout.valid && !p->acquire_count) {
			plane = p;
			free_planes++;
		}
	}

	if (free_planes == 0) {
		// No available plane for scanout
		return NULL;
	}

	if (free_planes == 1 && view->link.prev != &ec->view_list) {
		// Only one available plane left, and this is not the last
		// view in the list. We might need the plane for the
		// composited framebuffer so do not use it now.
		return NULL;
	}

	if (ice_plane_assign_graphics(plane, view)) {
		// Could not use view buffer for scanout
		return NULL;
	}

	return &plane->base;
}

static struct weston_plane *
ice_output_assign_plane(struct ice_output *output, struct weston_view *view,
			pixman_region32_t *composited_region)
{
	struct weston_compositor *ec = output->base.compositor;
	struct ice_backend *backend = ice_backend(ec);
	struct weston_plane *plane;
	struct weston_plane *primary;
	struct weston_surface *surface;
	pixman_region32_t overlap;

	primary = &ec->primary_plane;

	if (view->layer_link.layer == &backend->background_layer) {
		// dummy layer to track composited framebuffer damage
		return primary;
	}

	if (view->layer_link.layer == &ec->cursor_layer) {
		if ((plane = ice_output_assign_cursor_view(output, view)))
			return plane;
	}

	surface = view->surface;
	surface->keep_buffer = surface->buffer_ref.buffer &&
		!wl_shm_buffer_get(surface->buffer_ref.buffer->resource);

	pixman_region32_init(&overlap);
	pixman_region32_intersect(&overlap, composited_region,
				  &view->transform.boundingbox);

	plane = ice_output_assign_sideband_view(output, view);
	if (plane == NULL && pixman_region32_not_empty(&overlap))
		plane = primary;
	if (plane == NULL)
		plane = ice_output_assign_graphics_view(output, view);
	if (plane == NULL)
		plane = primary;

	pixman_region32_fini(&overlap);

	if (plane == primary)
		pixman_region32_union(composited_region, composited_region,
				      &view->transform.boundingbox);

	if (plane == primary || plane == &output->cursor_plane.base) {
		/* cursor plane involves a copy */
		view->psf_flags = 0;
	} else {
		/* all other planes scanout the client buffer directly */
		view->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
	}

	return plane;
}

static void
ice_output_assign_planes(struct weston_output *output)
{
	struct ice_output *iceout = ice_output(output);
	struct weston_compositor *compositor = output->compositor;
	struct weston_plane *plane;
	struct weston_view *ev;
	pixman_region32_t composited_region;

	dbg("assign planes\n");

	/*
	 * Planes are layed out the following way (top first):
	 *  . cursor plane (IAP plane)
	 *  . primary plane (GPU rendered framebuffer, flipped on UPP plane)
	 *  . direct rendered surfaces (UPP planes)
	 *
	 * The composited framebuffer is stacked on top of hardware planes,
	 * because we usually have video in the background, which cannot
	 * be composited using the GPU with the SRB api.
	 *
	 * Views are assigned to planes starting at the bottom of the stack.
	 * Unfortunately we cannot easily know if the view is hidden or
	 * overlapped, so we might be assigning a plane for nothing. It is
	 * assumed the TV shell does not stack windows on each other.
	 *
	 * The framebuffer background needs to be transparent and the clear
	 * damage must be tracked. For that we use the transparent and opaque
	 * view that was added to the background layer; it is moved at the
	 * back off the primary plane.
	 */

	pixman_region32_init(&composited_region);

	wl_list_for_each_reverse(ev, &compositor->view_list, link) {
		plane = ice_output_assign_plane(iceout, ev, &composited_region);
		weston_view_move_to_plane(ev, plane);
	}

	pixman_region32_fini(&composited_region);
}

static int refresh_table[] = {
	[GDL_REFRESH_23_98]	= 23976,
	[GDL_REFRESH_24]	= 24000,
	[GDL_REFRESH_25]	= 25000,
	[GDL_REFRESH_29_97]	= 29970,
	[GDL_REFRESH_30]	= 30000,
	[GDL_REFRESH_47_96]	= 47952,
	[GDL_REFRESH_48]	= 48000,
	[GDL_REFRESH_50]	= 50000,
	[GDL_REFRESH_59_94]	= 59940,
	[GDL_REFRESH_60]	= 60000,
	[GDL_REFRESH_85]	= 85000,
	[GDL_REFRESH_100]	= 100000,
	[GDL_REFRESH_119_88]	= 119880,
	[GDL_REFRESH_120]	= 120000,
};

static uint32_t
gdl_refresh_to_wayland(gdl_refresh_t refresh)
{
	if (refresh >= ARRAY_LENGTH(refresh_table))
		return 0;

	return refresh_table[refresh];
}

static struct ice_mode *
ice_output_add_tvmode(struct ice_output *output, const gdl_tvmode_t *tvmode)
{
	struct ice_mode *mode;

	mode = zalloc(sizeof *mode);
	if (mode == NULL)
		return NULL;

	mode->base.flags = 0;
	mode->base.refresh = gdl_refresh_to_wayland(tvmode->refresh);
	mode->base.width = tvmode->width;
	mode->base.height = tvmode->height;
	mode->interlaced = tvmode->interlaced;

	wl_list_insert(output->base.mode_list.prev, &mode->base.link);

	return mode;
}

static struct ice_mode *
ice_output_find_tvmode(struct ice_output *output, const gdl_tvmode_t *tvmode)
{
	uint32_t refresh;
	struct ice_mode *mode;

	refresh = gdl_refresh_to_wayland(tvmode->refresh);

	wl_list_for_each(mode, &output->base.mode_list, base.link) {
		if (mode->base.width == (int)tvmode->width &&
		    mode->base.height == (int)tvmode->height &&
		    mode->base.refresh == refresh &&
		    mode->interlaced == output->tvmode.interlaced)
			return mode;
	}

	return NULL;
}

static void
ice_output_clear_modes(struct ice_output *output)
{
	struct ice_mode *mode, *next;

	wl_list_for_each_safe(mode, next, &output->base.mode_list, base.link) {
		wl_list_remove(&mode->base.link);
		free(mode);
	}

	output->base.current_mode = NULL;
}

static int
ice_output_switch_mode(struct weston_output *base,
		       struct weston_mode *base_mode)
{
	struct ice_output *output = ice_output(base);
	struct ice_mode *mode;

	if (!(base_mode->flags & ICE_OUTPUT_MODE_TVMODE)) {
		// only allow internal display mode switches
		return -1;
	}

	mode = ice_output_find_tvmode(output, &output->tvmode);
	if (mode && (&mode->base == output->base.current_mode))
		return 0;

	ice_output_fini(output);
	ice_output_clear_modes(output);

	mode = ice_output_add_tvmode(output, &output->tvmode);
	output->base.current_mode = &mode->base;
	output->base.current_mode->flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	weston_log("switch mode to %dx%d%c%.1f\n",
		   mode->base.width, mode->base.height,
		   mode->interlaced ? 'i' : 'p',
		   mode->base.refresh / 1000.0f);

	weston_output_damage(&output->base);

	return ice_output_init(output);
}

static void
ice_output_destroy(struct weston_output *base)
{
	struct ice_output *output = ice_output(base);
	int i;

	ice_output_fini_vblank(output);
	ice_output_fini(output);
	ice_output_clear_modes(output);

	for (i = 0; i < output->num_planes; i++)
		weston_plane_release(&output->planes[i].base);

	weston_plane_release(&output->cursor_plane.base);
	weston_output_destroy(&output->base);

	free(output);
}

static void *
ice_output_wait_vblank(void *data)
{
	struct ice_output *output = data;
	struct timespec ts;

	while (output->vblank_source) {
		gdl_display_wait_for_vblank(GDL_DISPLAY_ID_0, NULL);

		// manufacture flip completion timestamp
		weston_compositor_read_presentation_clock(
			output->base.compositor, &ts);

		write(output->vblank_pipe[1], &ts, sizeof ts);
	}

	return NULL;
}

static int
ice_output_handle_vblank(int fd, uint32_t mask, void *data)
{
	struct ice_output *output = data;
	struct timespec ts;
	int vblanks;
	int ret;

	vblanks = 0;
	while ((ret = read(fd, &ts, sizeof ts)) == sizeof ts)
		vblanks++;

	if (ret != sizeof ts && errno != EAGAIN) {
		weston_log("vblank pipe read failed: %m");
		return 0;
	}

	assert(vblanks > 0);

	if (output->flip_pending &&
	    timespec_cmp(&output->flip_ts, &ts) < 0) {
		int i;

		dbg("vblank after flip\n");
		ret = 0;

		for (i = 0; i < output->num_planes; i++)
			ret |= ice_plane_finish_flip(&output->planes[i]);

		ice_plane_finish_flip(&output->cursor_plane);

		if (!ret) {
			output->flip_pending = 0;
			output->finish_frame = 1;
		}
	}

	if (output->finish_frame && !output->flip_pending) {
		uint32_t flags = WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION |
				 WP_PRESENTATION_FEEDBACK_KIND_VSYNC;

		dbg("finish frame\n");
		weston_output_finish_frame(&output->base, &ts, flags);
		output->finish_frame = 0;
	}

	return 0;
}

static void
ice_output_fini_vblank(struct ice_output *output)
{
	if (output->vblank_source) {
		wl_event_source_remove(output->vblank_source);
		output->vblank_source = NULL;
		pthread_cancel(output->vblank_tid);
		pthread_join(output->vblank_tid, NULL);
		close(output->vblank_pipe[0]);
		close(output->vblank_pipe[1]);
	}
}

static int
ice_output_init_vblank(struct ice_output *output)
{
	struct weston_compositor *ec = output->base.compositor;
	struct wl_event_loop *loop;

	if (output->vblank_source != NULL)
		return 0;

	if (pipe2(output->vblank_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		weston_log("failed to create pipe for vblank: %m\n");
		return -1;
	}

	loop = wl_display_get_event_loop(ec->wl_display);

	output->vblank_source =
		wl_event_loop_add_fd(loop, output->vblank_pipe[0],
				     WL_EVENT_READABLE,
				     ice_output_handle_vblank, output);

	if (!output->vblank_source) {
		close(output->vblank_pipe[0]);
		close(output->vblank_pipe[1]);
		return -1;
	}

	if (pthread_create(&output->vblank_tid, NULL,
			   ice_output_wait_vblank, output)) {
		weston_log("failed to create thread for vblank: %m");
		return -1;
	}

	return 0;
}

static struct ice_output *
create_output(struct ice_backend *b, gdl_display_id_t disp_id,
	      const gdl_tvmode_t *tvmode)
{
	struct weston_compositor *ec = b->compositor;
	struct ice_output *output;
	struct ice_mode *mode;
	char name[32];
	int i;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;

	output->disp_id = disp_id;
	output->tvmode = *tvmode;

	output->base.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
	output->base.make = "Freebox";
	output->base.model = "Revolution";
	output->base.serial_number = "unknown";

	snprintf(name, sizeof name, "Display Pipe %d", disp_id);
	output->base.name = strdup(name);

	wl_list_init(&output->base.mode_list);

	mode = ice_output_add_tvmode(output, tvmode);
	mode->base.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->base.current_mode = &mode->base;

	weston_output_init(&output->base, ec, 0, 0, 0, 0,
			   WL_OUTPUT_TRANSFORM_NORMAL, 1);

	ice_plane_init(&output->cursor_plane, GDL_PLANE_ID_IAP_B, output);
	weston_compositor_stack_plane(ec, &output->cursor_plane.base, NULL);

	output->scaled_plane = GDL_PLANE_ID_UNDEFINED;
	output->num_planes = ARRAY_LENGTH(output->planes);

	for (i = 0; i < output->num_planes; i++) {
		struct weston_plane *last_plane =
			wl_container_of(&ec->plane_list, last_plane, link);

		ice_plane_init(&output->planes[i],
			       GDL_PLANE_ID_UPP_A + i, output);

		weston_compositor_stack_plane(ec, &output->planes[i].base,
					      last_plane);
	}

	if (ice_output_init(output) < 0) {
		weston_log("failed to init output\n");
		goto err_output;
	}

	weston_compositor_add_output(ec, &output->base);

	output->base.start_repaint_loop = ice_output_start_repaint_loop;
	output->base.assign_planes = ice_output_assign_planes;
	output->base.repaint = ice_output_repaint;
	output->base.switch_mode = ice_output_switch_mode;
	output->base.destroy = ice_output_destroy;
	//output->base.disable_planes = 1;

	weston_log("%s\n", name);
	wl_list_for_each(mode, &output->base.mode_list, base.link) {
		int preferred = mode->base.flags & WL_OUTPUT_MODE_PREFERRED;
		int current = mode->base.flags & WL_OUTPUT_MODE_CURRENT;

		weston_log_continue("  mode %dx%d%c%.1f%s%s\n",
				    mode->base.width, mode->base.height,
				    mode->interlaced ? 'i' : 'p',
				    mode->base.refresh / 1000.0,
				    preferred ? ", preferred" : "",
				    current ? ", current" : "");
	}

	return output;

err_output:
	weston_output_destroy(&output->base);
	ice_output_clear_modes(output);
	free(output);
	return NULL;
}

static int
update_display_mode(struct ice_backend *b)
{
	struct weston_compositor *ec = b->compositor;
	struct ice_output *output;
	gdl_display_id_t disp_id;
	gdl_display_info_t disp_info;
	gdl_ret_t rc;

	disp_id = GDL_DISPLAY_ID_0;

	rc = gdl_get_display_info(disp_id, &disp_info);
	if (rc == GDL_ERR_TVMODE_UNDEFINED) {
		disp_info.tvmode.width = 720;
		disp_info.tvmode.height = 576;
		disp_info.tvmode.refresh = GDL_REFRESH_50;
		disp_info.tvmode.interlaced = GDL_FALSE;
		disp_info.tvmode.stereo_type = GDL_STEREO_NONE;

	} else if (rc != GDL_SUCCESS) {
		weston_log("failed to get display info for pipe %d: %s\n",
			   disp_id, gdl_get_error_string(rc));
		return -1;
	}

	if (wl_list_empty(&ec->output_list)) {
		output = create_output(b, GDL_DISPLAY_ID_0, &disp_info.tvmode);
		if (output == NULL)
			return -1;
	} else {
		struct weston_mode mode;

		output = wl_container_of(ec->output_list.next,
					 output, base.link);

		output->tvmode = disp_info.tvmode;

		mode.flags = ICE_OUTPUT_MODE_TVMODE;
		mode.width = disp_info.tvmode.width;
		mode.height = disp_info.tvmode.height;
		mode.refresh = gdl_refresh_to_wayland(disp_info.tvmode.refresh);

		if (weston_output_mode_set_native(&output->base, &mode, 1))
			return -1;
	}

	if (rc != GDL_ERR_TVMODE_UNDEFINED && ice_output_init_vblank(output))
		return -1;

	return 0;

}

static int
dispatch_gdl_event(int fd, uint32_t mask, void *data)
{
	struct ice_backend *b = data;
	uint64_t v;

	if (read(b->gdl_event_fd, &v, sizeof v) < 0)
		return 0;

	update_display_mode(b);

	return 0;
}

static void
handle_gdl_event(gdl_app_event_t event, void *data)
{
	struct ice_backend *b = data;
	uint64_t v = 1;

	write(b->gdl_event_fd, &v, sizeof v);
}

static int
create_outputs(struct ice_backend *b)
{
	struct wl_event_loop *loop;
	int fd;
	gdl_ret_t rc;

	loop = wl_display_get_event_loop(b->compositor->wl_display);

	fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0) {
		weston_log("failed to create gdl event fd: %m");
		return -1;
	}

	rc = gdl_event_register(GDL_APP_EVENT_MODE_DISP_0,
				handle_gdl_event, b);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to register gdl display mode event\n");
		close(fd);
		return -1;
	}

	b->gdl_event_fd = fd;
	b->gdl_event_source = wl_event_loop_add_fd(loop, b->gdl_event_fd,
						   WL_EVENT_READABLE,
						   dispatch_gdl_event, b);

	update_display_mode(b);

	return 0;
}

static void
buffer_handle_destroy(struct wl_resource *resource)
{
	void *buffer = wl_resource_get_user_data(resource);

	free(buffer);
}

static void
buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface gdl_buffer_interface = {
	buffer_destroy,
};

static void
create_buffer(struct wl_client *client, struct wl_resource *resource,
	      uint32_t id, uint32_t name, gdl_color_space_t color_space)
{
	struct wl_gdl_buffer *buffer;

	buffer = malloc(sizeof (*buffer));
	if (!buffer) {
		wl_resource_post_no_memory(resource);
		return;
	}

	if (gdl_get_surface_info(name, &buffer->surface_info) != GDL_SUCCESS) {
		memset(&buffer->surface_info, 0, sizeof (buffer->surface_info));
		buffer->surface_info.id = GDL_SURFACE_INVALID;
	}

	buffer->color_space = color_space;

	buffer->resource =
		wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (buffer->resource == NULL) {
		wl_resource_post_no_memory(resource);
		free(buffer);
		return;
	}

	wl_resource_set_implementation(buffer->resource,
				       &gdl_buffer_interface,
				       buffer, buffer_handle_destroy);
}

static const struct wl_buffer_interface gdl_sideband_buffer_interface = {
	buffer_destroy,
};

static void
create_sideband_buffer_type(struct wl_client *client,
			    struct wl_resource *resource, uint32_t id,
			    struct ice_plane *plane,
			    enum ice_sideband_type type,
			    uint32_t width, uint32_t height)
{
	struct wl_gdl_sideband_buffer *buffer;

	buffer = malloc(sizeof (*buffer));
	if (!buffer) {
		wl_resource_post_no_memory(resource);
		return;
	}

	buffer->type = type;
	buffer->plane = plane;
	buffer->width = width;
	buffer->height = height;

	buffer->resource =
		wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buffer->resource) {
		wl_resource_post_no_memory(resource);
		free(buffer);
		return;
	}

	wl_resource_set_implementation(buffer->resource,
				       &gdl_sideband_buffer_interface,
				       buffer, buffer_handle_destroy);
}

static void
create_sideband_buffer(struct wl_client *client, struct wl_resource *resource,
		       uint32_t id, uint32_t plane_id,
		       uint32_t width, uint32_t height)
{
	struct ice_backend *b = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = b->compositor;
	struct ice_output *output;
	struct ice_plane *plane;

	if (wl_list_empty(&ec->output_list))
		output = NULL;
	else
		output = wl_container_of(ec->output_list.next,
					 output, base.link);

	if (!output || !(plane = ice_output_find_plane(output, plane_id))) {
		wl_resource_post_error(resource, WL_GDL_ERROR_INVALID_PLANE,
				       "invalid gdl plane %u", plane_id);
		return;
	}

	create_sideband_buffer_type(client, resource, id, plane,
				    ICE_SIDEBAND_VIDEO, width, height);
}

static void
plane_get_buffer(struct wl_client *client, struct wl_resource *resource,
		 uint32_t id, uint32_t width, uint32_t height)
{
	struct ice_plane *plane = wl_resource_get_user_data(resource);

	create_sideband_buffer_type(client, resource, id, plane,
				    ICE_SIDEBAND_BYPASS, width, height);
}

static void
plane_handle_destroy(struct wl_resource *resource)
{
	struct ice_plane *plane = wl_resource_get_user_data(resource);

	plane->acquire_count--;

	if (plane->acquire_count == 0) {
		dbg("plane %s released\n", plane->name);
		ice_plane_reset_config(plane);
	}
}

static void
plane_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_gdl_plane_interface gdl_plane_interface = {
	plane_get_buffer,
	plane_release,
};

static void
acquire_plane(struct wl_client *client, struct wl_resource *resource,
	      uint32_t id, uint32_t plane_id)
{
	struct ice_backend *b = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = b->compositor;
	struct ice_output *output;
	struct ice_plane *plane;
	struct wl_resource *plane_resource;

	if (wl_list_empty(&ec->output_list))
		output = NULL;
	else
		output = wl_container_of(ec->output_list.next,
					 output, base.link);

	if (!output || !(plane = ice_output_find_plane(output, plane_id))) {
		wl_resource_post_error(resource, WL_GDL_ERROR_INVALID_PLANE,
				       "invalid gdl plane %u", plane_id);
		return;
	}

	plane_resource =
		wl_resource_create(client, &wl_gdl_plane_interface, 1, id);
	if (!plane_resource) {
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(plane_resource, &gdl_plane_interface,
				       plane, plane_handle_destroy);

	if (plane->acquire_count == 0) {
		dbg("plane %s acquired\n", plane->name);
		ice_plane_reset_config(plane);
	}

	plane->acquire_count++;
}

static const struct wl_gdl_interface gdl_interface = {
	create_buffer,
	create_sideband_buffer,
	acquire_plane,
};

static void
bind_gdl(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_gdl_interface,
				      MIN(version, 2), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &gdl_interface, data, NULL);
}

struct wl_gdl_buffer *
wl_gdl_buffer_get(struct wl_resource *resource)
{
	if (wl_resource_instance_of(resource, &wl_buffer_interface,
				    &gdl_buffer_interface))
		return wl_resource_get_user_data(resource);
	else
		return NULL;
}

gdl_surface_info_t *
wl_gdl_buffer_get_surface_info(struct wl_gdl_buffer *buffer)
{
	return &buffer->surface_info;
}

struct wl_gdl_sideband_buffer *
wl_gdl_sideband_buffer_get(struct wl_resource *resource)
{
	if (wl_resource_instance_of(resource, &wl_buffer_interface,
				    &gdl_sideband_buffer_interface))
		return wl_resource_get_user_data(resource);
	else
		return NULL;
}

uint32_t
wl_gdl_sideband_buffer_get_width(struct wl_gdl_sideband_buffer *buffer)
{
	return buffer->width;
}

uint32_t
wl_gdl_sideband_buffer_get_height(struct wl_gdl_sideband_buffer *buffer)
{
	return buffer->height;
}

static void
planes_binding(struct weston_keyboard *keyboard, uint32_t time,
	       uint32_t key, void *data)
{
	struct ice_backend *b = data;
	struct weston_compositor *ec = b->compositor;
	struct weston_output *output;

	switch (key) {
	case KEY_P:
		b->debug_planes ^= 1;
		weston_compositor_schedule_repaint(b->compositor);
		break;
	case KEY_C:
		wl_list_for_each(output, &ec->output_list, link)
			output->disable_planes = !output->disable_planes;
		weston_compositor_schedule_repaint(b->compositor);
		break;
	default:
		break;
	}
}

static int
init_pixman(struct ice_backend *b)
{
	if (pixman_renderer_init(b->compositor) < 0) {
		weston_log("failed to initialize pixman renderer\n");
		return -1;
	}

	return 0;
}

static int
init_srb(struct ice_backend *b)
{
	if (ice_renderer_init(b->compositor) < 0) {
		weston_log("failed to initialize srb renderer\n");
		return -1;
	}

	return 0;
}

static int
init_gdl(struct ice_backend *b)
{
	gdl_ret_t rc;
	gdl_driver_info_t info;

	rc = gdl_init(0);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to initialize gdl: %s\n",
			   gdl_get_error_string(rc));
		return -1;
	}

	if (gdl_get_driver_info(&info) == GDL_SUCCESS)
		weston_log("%s version %d.%d.%d, "
			   "%u KB total memory\n", info.name,
			   GET_GDL_VERSION_MAJOR(info.gdl_version),
			   GET_GDL_VERSION_MINOR(info.gdl_version),
			   GET_GDL_VERSION_HOTFIX(info.gdl_version),
			   info.mem_size / 1024);

	if (!wl_global_create(b->compositor->wl_display, &wl_gdl_interface, 2,
			      b, bind_gdl)) {
		gdl_close();
		return -1;
	}

	return 0;
}

static int
create_background(struct ice_backend *b)
{
	struct weston_surface *surface;
	struct weston_view *view;

	surface = weston_surface_create(b->compositor);
	if (!surface)
		return -1;

	view = weston_view_create(surface);
	if (!view) {
		weston_surface_destroy(surface);
		return -1;
	}

	weston_surface_set_color(surface, 0, 0, 0, 0);
	weston_surface_set_size(surface, 8192, 8192);
	pixman_region32_init_rect(&surface->opaque, 0, 0, 8192, 8192);
	pixman_region32_init(&surface->input);

	weston_view_set_position(view, 0, 0);
	view->surface->is_mapped = true;
	view->is_mapped = true;

	weston_layer_init(&b->background_layer,
			  &b->compositor->cursor_layer.link);
	weston_layer_entry_insert(&b->background_layer.view_list,
				  &view->layer_link);

	b->background_surface = surface;
	b->background_view = view;

	return 0;
}

static struct ice_backend *
ice_backend_create(struct weston_compositor *compositor,
		   struct weston_ice_backend_config *config)
{
	struct ice_backend *b;

	weston_log("initializing IntelCE backend\n");

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	if (weston_compositor_set_presentation_clock_software(compositor) < 0)
		goto err_compositor;

	compositor->backend = &b->base;

	b->compositor = compositor;
	b->gdl_event_fd = -1;

	if (init_gdl(b) < 0)
		goto err_compositor;

	if ((b->use_pixman = config->use_pixman)) {
		if (init_pixman(b) < 0)
			goto err_gdl;
	} else {
		if (init_srb(b) < 0)
			goto err_gdl;
	}

	if (create_background(b) < 0) {
		weston_log("failed to create background surface");
		goto err_gdl;
	}

	if (input_lh_init(&b->input, compositor) < 0) {
		weston_log("failed to create input devices\n");
		goto err_gdl;
	}

	b->base.destroy = ice_destroy;
	b->base.restore = ice_restore;

	if (create_outputs(b) < 0)
		goto err_gdl;

	weston_compositor_add_debug_binding(compositor, KEY_P,
					    planes_binding, b);
	weston_compositor_add_debug_binding(compositor, KEY_C,
					    planes_binding, b);

	return b;

err_gdl:
	gdl_close();
err_compositor:
	weston_compositor_shutdown(compositor);
	free(b);
	return NULL;
}

WL_EXPORT int
backend_init(struct weston_compositor *compositor,
	     struct weston_backend_config *config_base)
{
	struct weston_ice_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_ICE_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_ice_backend_config)) {
		weston_log("ice backend config structure is invalid\n");
		return -1;
	}

	memcpy(&config, config_base, config_base->struct_size);

	return ice_backend_create(compositor, &config) ? 0 : -1;
}
