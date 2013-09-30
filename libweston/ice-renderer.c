#include <limits.h>
#include <string.h>

#include <libgma.h>
#include <libgdl.h>
#include <srb.h>
#include <x86_cache.h>

#include "ice-renderer.h"
#include "gdl-buffer.h"
#include "hash.h"
#include "shared/helpers.h"

//#define DEBUG

struct ice_buffer_state {
	struct ice_renderer *renderer;
	srb_surface_t surface;
	struct wl_listener destroy_listener;
	int width;
	int height;
	int ref_count;
};

struct ice_renderer_fb {
	srb_surface_t surface;
	gma_pixmap_t pixmap;
};

struct ice_output_state {
	struct ice_renderer_fb *fb;
};

struct ice_surface_state {
	struct ice_buffer_state *buffer_state;
	srb_color_t color;
	int color_set;
	struct weston_buffer_reference buffer_ref;
	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;
};

struct ice_renderer {
	struct weston_renderer base;
	srb_context_t srb;
	srb_device_info_t *device_info;
	struct wl_signal destroy_signal;
	struct hash_table *buffer_ht;
};

static int ice_renderer_create_surface(struct weston_surface *surface);

#ifdef DEBUG
# define dbg(fmt, ...) weston_log(fmt, ##__VA_ARGS__)
#else
# define dbg(fmt, ...) ({})
#endif

static inline struct ice_renderer *
get_renderer(struct weston_compositor *compositor)
{
	return container_of(compositor->renderer, struct ice_renderer, base);
}

static inline struct ice_output_state *
get_output_state(struct weston_output *output)
{
	return (struct ice_output_state *)output->renderer_state;
}

static inline struct ice_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		ice_renderer_create_surface(surface);

	return (struct ice_surface_state *)surface->renderer_state;
}

static inline void
weston_vector_init_2d(struct weston_vector *v, float x, float y)
{
	v->f[0] = x;
	v->f[1] = y;
	v->f[2] = 0.0f;
	v->f[3] = 1.0f;
}

static inline uint32_t
premul_argb_color(uint32_t color, float alpha)
{
	uint32_t a = (color >> 24) * alpha;

	return (a << 24) |
		((((color >> 8) & 0xff) * (a + 1)) & 0xff00) |
		((((color & 0x00ff00ff) * (a + 1)) >> 8) & 0x00ff00ff);
}

static void
fill_region(struct weston_view *ev, struct weston_output *output,
	    pixman_region32_t *region)
{
	struct ice_renderer *renderer = get_renderer(output->compositor);
	struct ice_surface_state *ps = get_surface_state(ev->surface);
	struct ice_output_state *po = get_output_state(output);
	struct weston_matrix matrix;
	struct weston_vector p1, p2;
	pixman_box32_t *rects;
	int i, nrects;
	srb_fill_info_t fill;
	gdl_ret_t rc;

	if (ev->alpha < 1.0) {
		fill.blend.flags = SRB_BLEND_ENABLE_BLEND_EQUATION;
		fill.blend.src_rgb = SRB_BLEND_FUNC_ONE;
		fill.blend.src_alpha = SRB_BLEND_FUNC_ONE;
		fill.blend.dest_rgb = SRB_BLEND_FUNC_ONE_MINUS_SRC;
		fill.blend.dest_alpha = SRB_BLEND_FUNC_ONE_MINUS_SRC;
	} else
		fill.blend.flags = 0;

	fill.fill_color = premul_argb_color(ps->color, ev->alpha);
	fill.fill_surface_handle = &po->fb->surface;

	matrix = ev->transform.matrix;
	weston_matrix_multiply(&matrix, &output->matrix);

	dbg("fill color=%08x alpha=%.2f\n", ps->color, ev->alpha);

	rects = pixman_region32_rectangles(region, &nrects);

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];

		weston_vector_init_2d(&p1, rect->x1, rect->y1);
		weston_matrix_transform(&output->matrix, &p1);

		weston_vector_init_2d(&p2, rect->x2, rect->y2);
		weston_matrix_transform(&output->matrix, &p2);

		fill.fill_rect.width = nearbyint(p2.f[0] - p1.f[0]);
		fill.fill_rect.height = nearbyint(p2.f[1] - p1.f[1]);
		fill.fill_rect.origin.x = nearbyint(p1.f[0]);
		fill.fill_rect.origin.y = nearbyint(p1.f[1]);
		fill.clip_rect = fill.fill_rect;

		dbg(" . output %dx%d%+d%+d\n",
		    fill.fill_rect.width, fill.fill_rect.height,
		    fill.fill_rect.origin.x, fill.fill_rect.origin.y);

		rc = srb_fill(&renderer->srb, &fill);
		if (rc != GDL_SUCCESS)
			weston_log("fill failed\n");
	}
}

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))

static inline int
fixed_to_int(wl_fixed_t f)
{
	return nearbyint(wl_fixed_to_double(f));
}

static void
blit_region(struct weston_view *ev, struct weston_output *output,
	    pixman_region32_t *region, pixman_region32_t *surf_region,
	    pixman_op_t pixman_op)
{
	struct ice_renderer *renderer = get_renderer(output->compositor);
	struct ice_surface_state *ps = get_surface_state(ev->surface);
	struct ice_output_state *po = get_output_state(output);
	struct weston_surface *surface = ev->surface;
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	struct weston_matrix matrix;
	struct weston_vector p1, p2;
	pixman_box32_t *rects, *surf_rects;
	int i, j, nrects, nsurf;
	srb_blit_info_t blit;
	gdl_ret_t rc;

	rects = pixman_region32_rectangles(region, &nrects);
	surf_rects = pixman_region32_rectangles(surf_region, &nsurf);

	if (pixman_op == PIXMAN_OP_OVER || ev->alpha < 1.0) {
		blit.blend.flags = SRB_BLEND_ENABLE_BLEND_EQUATION;
		blit.blend.src_rgb = SRB_BLEND_FUNC_ONE;
		blit.blend.src_alpha = SRB_BLEND_FUNC_ONE;
		blit.blend.dest_rgb = SRB_BLEND_FUNC_ONE_MINUS_SRC;
		blit.blend.dest_alpha = SRB_BLEND_FUNC_ONE_MINUS_SRC;
	} else
		blit.blend.flags = 0;

	if (ev->alpha < 1.0) {
		uint8_t a = ev->alpha * 0xff;
		blit.blend.flags |= SRB_BLEND_ENABLE_SRC_MODULATE;
		blit.blend.modulation_color =
			(a << 24) | (a << 16) | (a << 8) | a;
	}

	blit.src_surface_handle = &ps->buffer_state->surface;
	if (vp->buffer.src_width != wl_fixed_from_int(-1)) {
		blit.src_rect.width = fixed_to_int(vp->buffer.src_width);
		blit.src_rect.height = fixed_to_int(vp->buffer.src_height);
		blit.src_rect.origin.x = fixed_to_int(vp->buffer.src_x);
		blit.src_rect.origin.y = fixed_to_int(vp->buffer.src_y);
	} else {
		blit.src_rect.width = surface->width_from_buffer;
		blit.src_rect.height = surface->height_from_buffer;
		blit.src_rect.origin.x = 0;
		blit.src_rect.origin.y = 0;
	}

	if (vp->buffer.scale > 1) {
		blit.src_rect.width *= vp->buffer.scale;
		blit.src_rect.height *= vp->buffer.scale;
		blit.src_rect.origin.x *= vp->buffer.scale;
		blit.src_rect.origin.y *= vp->buffer.scale;
	}

	matrix = ev->transform.matrix;
	weston_matrix_multiply(&matrix, &output->matrix);

	weston_vector_init_2d(&p1, 0.0f, 0.0f);
	weston_matrix_transform(&matrix, &p1);

	weston_vector_init_2d(&p2, surface->width, surface->height);
	weston_matrix_transform(&matrix, &p2);

	blit.dest_surface_handle = &po->fb->surface;
	blit.dest_rect.width = nearbyintf(p2.f[0] - p1.f[0]);
	blit.dest_rect.height = nearbyintf(p2.f[1] - p1.f[1]);
	blit.dest_rect.origin.x = nearbyintf(p1.f[0]);
	blit.dest_rect.origin.y = nearbyintf(p1.f[1]);

	if (blit.dest_rect.width != blit.src_rect.width ||
	    blit.dest_rect.height != blit.src_rect.height)
		blit.filter = SRB_FILTER_LINEAR;
	else
		blit.filter = SRB_FILTER_NEAREST;

	dbg("blit %s %dx%d%+d%+d -> %dx%d%+d%+d alpha=%.2f filter=%s\n",
	    pixman_op == PIXMAN_OP_SRC ? "src" :
	    pixman_op == PIXMAN_OP_OVER ? "over" : "??",
	    blit.src_rect.width, blit.src_rect.height,
	    blit.src_rect.origin.x, blit.src_rect.origin.y,
	    blit.dest_rect.width, blit.dest_rect.height,
	    blit.dest_rect.origin.x, blit.dest_rect.origin.y,
	    ev->alpha,
	    blit.filter == SRB_FILTER_NEAREST ? "nearest" :
	    blit.filter == SRB_FILTER_LINEAR ? "linear" : "??");

	dbg(" . buffer %dx%d, surface %dx%d from_buffer %dx%d\n",
	    ps->buffer_state->width, ps->buffer_state->height,
	    surface->width, surface->height,
	    surface->width_from_buffer, surface->height_from_buffer);

	if (vp->buffer.src_width != wl_fixed_from_int(-1))
		dbg(" . viewport %.2lfx%.2lf%+.2lf%+.2lf -> %dx%d "
		    "transform %d scale %d\n",
		    wl_fixed_to_double(vp->buffer.src_width),
		    wl_fixed_to_double(vp->buffer.src_height),
		    wl_fixed_to_double(vp->buffer.src_x),
		    wl_fixed_to_double(vp->buffer.src_y),
		    vp->surface.width, vp->surface.height,
		    vp->buffer.transform, vp->buffer.scale);

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];
		struct weston_vector rp1, rp2;

		weston_vector_init_2d(&rp1, rect->x1, rect->y1);
		weston_matrix_transform(&output->matrix, &rp1);

		weston_vector_init_2d(&rp2, rect->x2, rect->y2);
		weston_matrix_transform(&output->matrix, &rp2);

		dbg(" . output damage %dx%d%+d%+d\n",
		    (int)nearbyint(rp2.f[0] - rp1.f[0]),
		    (int)nearbyint(rp2.f[1] - rp1.f[1]),
		    (int)nearbyint(rp1.f[0]), (int)nearbyint(rp1.f[1]));

		for (j = 0; j < nsurf; j++) {
			pixman_box32_t *surf_rect = &surf_rects[j];

			weston_vector_init_2d(&p1, surf_rect->x1, surf_rect->y1);
			weston_matrix_transform(&matrix, &p1);

			weston_vector_init_2d(&p2, surf_rect->x2, surf_rect->y2);
			weston_matrix_transform(&matrix, &p2);

			p1.f[0] = floorf(max(p1.f[0], rp1.f[0]));
			p1.f[1] = floorf(max(p1.f[1], rp1.f[1]));
			p2.f[0] = ceilf(min(p2.f[0], rp2.f[0]));
			p2.f[1] = ceilf(min(p2.f[1], rp2.f[1]));

			blit.clip_rect.origin.x = p1.f[0];
			blit.clip_rect.origin.y = p1.f[1];
			blit.clip_rect.width = p2.f[0] - p1.f[0];
			blit.clip_rect.height = p2.f[1] - p1.f[1];

			dbg("   . surf %dx%d%+d%+d output %dx%d%+d%+d\n",
			    surf_rect->x2 - surf_rect->x1,
			    surf_rect->y2 - surf_rect->y1,
			    surf_rect->x1, surf_rect->y1,
			    blit.clip_rect.width, blit.clip_rect.height,
			    blit.clip_rect.origin.x, blit.clip_rect.origin.y);

			rc = srb_blit(&renderer->srb, &blit);
			if (rc != GDL_SUCCESS)
				weston_log("blit failed\n");
		}
	}
}

static void
draw_view(struct weston_view *ev, struct weston_output *output,
	  pixman_region32_t *damage)
{
	struct ice_surface_state *ps = get_surface_state(ev->surface);
	pixman_region32_t repaint;

	/* get repaint bounding region in global coordinates */
	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &ev->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &ev->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	if (!ps->color_set && ps->buffer_state) {
		pixman_region32_t surface_blend;

		/* get non-opaque region in surface coordinates */
		pixman_region32_init_rect(&surface_blend, 0, 0,
					  ev->surface->width,
					  ev->surface->height);

		pixman_region32_subtract(&surface_blend, &surface_blend,
					 &ev->surface->opaque);

		if (pixman_region32_not_empty(&ev->surface->opaque))
			blit_region(ev, output, &repaint, &ev->surface->opaque,
				    PIXMAN_OP_SRC);

		if (pixman_region32_not_empty(&surface_blend))
			blit_region(ev, output, &repaint, &surface_blend,
				    PIXMAN_OP_OVER);

		pixman_region32_fini(&surface_blend);
	} else {
		fill_region(ev, output, &repaint);
	}

out:
	pixman_region32_fini(&repaint);
}

static void
ice_renderer_repaint_output(struct weston_output *output,
			    pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct ice_renderer *renderer = get_renderer(compositor);
	struct ice_output_state *po = get_output_state(output);
	struct weston_view *view;

	if (!po->fb)
		return;

	wl_list_for_each_reverse(view, &compositor->view_list, link)
		if (view->plane == &compositor->primary_plane)
			draw_view(view, output, damage);

	srb_wait(&renderer->srb, &po->fb->surface);

	pixman_region32_copy(&output->previous_damage, damage);
	wl_signal_emit(&output->frame_signal, output);

	/* Actual flip should be done by caller */
}

void *
ice_renderer_create_framebuffer(struct weston_renderer *renderer,
				gdl_surface_info_t *surface_info,
				gdl_uint8 *data)
{
	struct ice_renderer *pr =
		container_of(renderer, struct ice_renderer, base);
	struct ice_renderer_fb *fb;
	gma_pixmap_info_t pixmap_info;
	gdl_ret_t rc;

	fb = zalloc(sizeof *fb);

	pixmap_info.type = GMA_PIXMAP_TYPE_PHYSICAL;
	pixmap_info.virt_addr = data;
	pixmap_info.phys_addr = surface_info->phys_addr;
	pixmap_info.format = GMA_PF_ARGB_32;
	pixmap_info.width = surface_info->width;
	pixmap_info.height = surface_info->height;
	pixmap_info.pitch = surface_info->pitch;
	pixmap_info.user_data = NULL;

	if (gma_pixmap_alloc(&pixmap_info, NULL, &fb->pixmap) != GMA_SUCCESS)
		goto fail;

	rc = srb_attach_pixmap(&pr->srb, fb->pixmap, &fb->surface);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to create srb surface: %s\n",
			   gdl_get_error_string(rc));
		goto fail;
	}

	return fb;

fail:
	if (fb->pixmap)
		gma_pixmap_release(&fb->pixmap);

	free(fb);

	return NULL;
}

void
ice_renderer_destroy_framebuffer(struct weston_renderer *renderer,
				 void *fb_data)
{
	struct ice_renderer *pr =
		container_of(renderer, struct ice_renderer, base);
	struct ice_renderer_fb *fb = fb_data;
	gdl_ret_t rc;

	if (fb) {
		rc = srb_detach_surface(&pr->srb, &fb->surface);
		if (rc != GDL_SUCCESS)
			weston_log("failed to detach fb surface: %s\n",
				   gdl_get_error_string(rc));

		rc = gma_pixmap_release(&fb->pixmap);
		if (rc != GDL_SUCCESS)
			weston_log("failed to release fb pixmap: %s\n",
				   gdl_get_error_string(rc));

		free(fb);
	}
}

void
ice_renderer_output_set_framebuffer(struct weston_output *output,
				    void *fb_data)
{
	struct ice_output_state *po = get_output_state(output);

	po->fb = fb_data;
}

int
ice_renderer_output_create(struct weston_output *output)
{
	struct ice_output_state *po;

	po = zalloc(sizeof *po);
	if (!po)
		return -1;

	output->renderer_state = po;

	return 0;
}

void
ice_renderer_output_destroy(struct weston_output *output)
{
	struct ice_output_state *po = get_output_state(output);

	free(po);
}

static void
ice_renderer_flush_damage(struct weston_surface *surface)
{
	struct wl_shm_buffer *shm_buffer;

	shm_buffer = wl_shm_buffer_get(surface->buffer_ref.buffer->resource);
	if (shm_buffer) {
		pixman_box32_t *e;
		pixman_box32_t r;
		uint8_t *pixels;
		int bpp;
		int width;
		int height;
		int stride;
		int i;

		e = pixman_region32_extents(&surface->damage);
		r = weston_surface_to_buffer_rect(surface, *e);

		switch (wl_shm_buffer_get_format(shm_buffer)) {
		case WL_SHM_FORMAT_XRGB8888:
		case WL_SHM_FORMAT_ARGB8888:
		case WL_SHM_FORMAT_ABGR8888:
			bpp = 4;
			break;
		case WL_SHM_FORMAT_RGB565:
		case WL_SHM_FORMAT_ARGB4444:
		case WL_SHM_FORMAT_ARGB1555:
			bpp = 2;
			break;
		default:
			weston_log("unknown shm buffer format\n");
			return;
		}

		width = r.x2 - r.x1;
		height = r.y2 - r.y1;

		stride = wl_shm_buffer_get_stride(shm_buffer);
		pixels = wl_shm_buffer_get_data(shm_buffer) +
			r.y1 * stride + r.x1 * bpp;

		dbg("flush %dx%d+%d%+d\n", width, height, r.x1, r.y1);

		wl_shm_buffer_begin_access(shm_buffer);
		for (i = 0; i < height; i++) {
			cache_flush_buffer(pixels, width * bpp);
			pixels += stride;
		}
		wl_shm_buffer_end_access(shm_buffer);
	}
}

static gma_ret_t
ice_renderer_destroy_pixmap(gma_pixmap_info_t *pixmap_info)
{
	if (pixmap_info->type == GMA_PIXMAP_TYPE_PHYSICAL) {
		gdl_surface_id_t id = (gdl_surface_id_t) pixmap_info->user_data;
		gdl_ret_t rc;

		rc = gdl_unmap_surface(id);
		if (rc != GDL_SUCCESS)
			weston_log("failed to unmap gdl surface %02u: %s\n",
				   id, gdl_get_error_string(rc));
		else
			dbg("unmapped gdl surface %02u\n", id);
	}

	return GMA_SUCCESS;
}

static int
ice_renderer_read_pixels(struct weston_output *output,
			 pixman_format_code_t format, void *pixels,
			 uint32_t x, uint32_t y,
			 uint32_t width, uint32_t height)
{
	struct ice_output_state *po = get_output_state(output);
	gma_pixmap_info_t pi;

	dbg("read pixels %ux%u%+d%+d\n", width, height, x, y);

	if (!po->fb)
		return -1;

	if (gma_pixmap_get_info(po->fb->pixmap, &pi) != GMA_SUCCESS)
		return -1;

	if (pi.format != GMA_PF_ARGB_32)
		return -1;

	pixman_blt(pi.virt_addr, pixels,
		   pi.pitch / 4, width,
		   32, PIXMAN_FORMAT_BPP(format),
		   x, y, 0, 0, width, height);

	return 0;
}

static void
ice_buffer_state_unref(struct ice_buffer_state **buffer_state)
{
	struct ice_buffer_state *bs = *buffer_state;

	if (!bs)
		return;

	if (--bs->ref_count <= 0) {
		srb_detach_surface(&bs->renderer->srb, &bs->surface);
		free(bs);
	}

	*buffer_state = NULL;
}

static struct ice_buffer_state *
ice_buffer_state_ref(struct ice_buffer_state *buffer_state)
{
	buffer_state->ref_count++;
	return buffer_state;
}

static void
ice_buffer_state_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct weston_buffer *buffer = data;
	struct ice_buffer_state *bs =
		container_of(listener, struct ice_buffer_state,
			     destroy_listener);

	hash_table_remove(bs->renderer->buffer_ht, (uint32_t)buffer->resource);
	ice_buffer_state_unref(&bs);
}

static gma_pixel_format_t
gma_pixel_format_from_gdl(gdl_pixel_format_t gdl_pf)
{
	switch (gdl_pf) {
	case GDL_PF_ARGB_32:
		return GMA_PF_ARGB_32;
	case GDL_PF_RGB_32:
		return GMA_PF_RGB_32;
	case GDL_PF_ARGB_16_1555:
		return GMA_PF_ARGB_16_1555;
	case GDL_PF_ARGB_16_4444:
		return GMA_PF_ARGB_16_4444;
	case GDL_PF_RGB_16:
		return GMA_PF_RGB_16;
	case GDL_PF_A8:
		return GMA_PF_A8;
	case GDL_PF_AY16:
		return GMA_PF_AY16;
	case GDL_PF_ABGR_32:
		return GMA_PF_ABGR_32;
	default:
		return GMA_PF_UNDEFINED;
	}
}

static gma_pixel_format_t
ice_renderer_get_source_pixel_format(struct ice_renderer *renderer,
				     gdl_pixel_format_t pixel_format)
{
	gma_pixel_format_t gma_pf;
	unsigned i;

	gma_pf = gma_pixel_format_from_gdl(pixel_format);
	if (gma_pf == GMA_PF_UNDEFINED)
		return gma_pf;

	for (i = 0; i < renderer->device_info->src_format_count; i++)
		if (renderer->device_info->src_formats[i] == gma_pf)
			return gma_pf;

	return GMA_PF_UNDEFINED;
}

static void
ice_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct ice_renderer *renderer = get_renderer(es->compositor);
	struct ice_surface_state *ps = get_surface_state(es);
	struct ice_buffer_state *bs;
	struct wl_shm_buffer *shm_buffer;
	struct wl_gdl_buffer *gdl_buffer;
	struct wl_gdl_sideband_buffer *gdl_sb_buffer;
	gma_pixmap_info_t pixmap_info;
	gma_pixmap_funcs_t pixmap_funcs;
	gma_pixmap_t pixmap;
	srb_surface_t surface;
	gdl_ret_t rc;

	weston_buffer_reference(&ps->buffer_ref, buffer);

	ps->color_set = 0;
	ps->color = 0xff000000;

	ice_buffer_state_unref(&ps->buffer_state);

	if (!buffer)
		return;

	bs = hash_table_lookup(renderer->buffer_ht, (uint32_t)buffer->resource);
	if (bs) {
		ps->buffer_state = ice_buffer_state_ref(bs);
		return;
	}

	if ((shm_buffer = wl_shm_buffer_get(buffer->resource))) {
		switch (wl_shm_buffer_get_format(shm_buffer)) {
		case WL_SHM_FORMAT_XRGB8888:
			pixmap_info.format = GMA_PF_RGB_32;
			break;
		case WL_SHM_FORMAT_ARGB8888:
			pixmap_info.format = GMA_PF_ARGB_32;
			break;
		case WL_SHM_FORMAT_ABGR8888:
			pixmap_info.format = GMA_PF_ABGR_32;
			break;
		case WL_SHM_FORMAT_RGB565:
			pixmap_info.format = GMA_PF_RGB_16;
			break;
		case WL_SHM_FORMAT_ARGB4444:
			pixmap_info.format = GMA_PF_ARGB_16_4444;
			break;
		case WL_SHM_FORMAT_ARGB1555:
			pixmap_info.format = GMA_PF_ARGB_16_1555;
			break;
		default:
			weston_log("unsupported shm buffer format\n");
			goto fail;
		}

		buffer->shm_buffer = shm_buffer;
		buffer->width = wl_shm_buffer_get_width(shm_buffer);
		buffer->height = wl_shm_buffer_get_height(shm_buffer);

		pixmap_info.type = GMA_PIXMAP_TYPE_VIRTUAL;
		pixmap_info.virt_addr = wl_shm_buffer_get_data(shm_buffer);
		pixmap_info.phys_addr = 0;
		pixmap_info.width = buffer->width;
		pixmap_info.height = buffer->height;
		pixmap_info.pitch = wl_shm_buffer_get_stride(shm_buffer);
		pixmap_info.user_data = NULL;

	} else if ((gdl_buffer = wl_gdl_buffer_get(buffer->resource))) {
		gdl_uint8 *data;
		gdl_surface_info_t *surface_info =
			wl_gdl_buffer_get_surface_info(gdl_buffer);

		buffer->width = surface_info->width;
		buffer->height = surface_info->height;

		pixmap_info.format = ice_renderer_get_source_pixel_format(
			renderer, surface_info->pixel_format);

		if (pixmap_info.format == GMA_PF_UNDEFINED) {
			// pixel format not supported by renderer
			return;
		}

		rc = gdl_map_surface(surface_info->id, &data, NULL);
		if (rc != GDL_SUCCESS) {
			weston_log("failed to map gdl surface: %s\n",
				   gdl_get_error_string(rc));
			goto fail;
		}

		dbg("mapped gdl surface %02u\n", surface_info->id);

		pixmap_info.type = GMA_PIXMAP_TYPE_PHYSICAL;
		pixmap_info.virt_addr = data;
		pixmap_info.phys_addr = surface_info->phys_addr;
		pixmap_info.width = surface_info->width;
		pixmap_info.height = surface_info->height;
		pixmap_info.pitch = surface_info->pitch;
		pixmap_info.user_data = (void *) surface_info->id;

	} else if ((gdl_sb_buffer =
		    wl_gdl_sideband_buffer_get(buffer->resource))) {
		buffer->width = wl_gdl_sideband_buffer_get_width(gdl_sb_buffer);
		buffer->height = wl_gdl_sideband_buffer_get_height(gdl_sb_buffer);
		return;

	} else {
		weston_log("unsupported buffer type\n");
		goto fail;
	}

	pixmap_funcs.destroy = ice_renderer_destroy_pixmap;

	gma_pixmap_alloc(&pixmap_info, &pixmap_funcs, &pixmap);
	rc = srb_attach_pixmap(&renderer->srb, pixmap, &surface);
	gma_pixmap_release(&pixmap);

	if (rc != GDL_SUCCESS) {
		weston_log("failed to create srb surface: %s\n",
			   gdl_get_error_string(rc));
		goto fail;
	}

	bs = malloc(sizeof *bs);
	bs->renderer = renderer;
	bs->surface = surface;
	bs->width = buffer->width;
	bs->height = buffer->height;
	bs->destroy_listener.notify = ice_buffer_state_handle_buffer_destroy;
	bs->ref_count = 1;

	wl_signal_add(&buffer->destroy_signal, &bs->destroy_listener);

	hash_table_insert(renderer->buffer_ht, (uint32_t)buffer->resource, bs);

	ps->buffer_state = ice_buffer_state_ref(bs);
	return;

fail:
	weston_buffer_reference(&ps->buffer_ref, NULL);
}

static void
surface_state_destroy(struct ice_surface_state *ps,
		      struct ice_renderer *renderer)
{
	wl_list_remove(&ps->surface_destroy_listener.link);
	wl_list_remove(&ps->renderer_destroy_listener.link);

	ice_buffer_state_unref(&ps->buffer_state);
	weston_buffer_reference(&ps->buffer_ref, NULL);
	free(ps);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_surface *surface = data;
	struct ice_renderer *renderer;
	struct ice_surface_state *ps;

	ps = container_of(listener, struct ice_surface_state,
			  surface_destroy_listener);

	renderer = get_renderer(surface->compositor);

	surface_state_destroy(ps, renderer);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct ice_renderer *renderer = data;
	struct ice_surface_state *ps;

	ps = container_of(listener, struct ice_surface_state,
			  renderer_destroy_listener);

	surface_state_destroy(ps, renderer);
}

static int
ice_renderer_create_surface(struct weston_surface *surface)
{
	struct ice_renderer *renderer;
	struct ice_surface_state *ps;

	ps = zalloc(sizeof *ps);
	if (!ps)
		return -1;

	surface->renderer_state = ps;

	ps->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &ps->surface_destroy_listener);

	renderer = get_renderer(surface->compositor);

	ps->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&renderer->destroy_signal,
		      &ps->renderer_destroy_listener);

	return 0;
}

static void
ice_renderer_surface_set_color(struct weston_surface *es,
			       float red, float green, float blue, float alpha)
{
	struct ice_surface_state *ps = get_surface_state(es);

	ice_buffer_state_unref(&ps->buffer_state);
	weston_buffer_reference(&ps->buffer_ref, NULL);

	ps->color_set = 1;
	ps->color = (((uint8_t)(alpha * 0xff)) << 24) |
		(((uint8_t)(red * 0xff)) << 16) |
		(((uint8_t)(green * 0xff)) << 8) |
		((uint8_t)(blue * 0xff));
}

static void
hash_destroy_buffer_state(void *element, void *data)
{
	struct ice_buffer_state *bs = element;

	ice_buffer_state_unref(&bs);
}

static void
ice_renderer_destroy(struct weston_compositor *ec)
{
	struct ice_renderer *renderer = get_renderer(ec);

	wl_signal_emit(&renderer->destroy_signal, renderer);

	hash_table_for_each(renderer->buffer_ht,
			    hash_destroy_buffer_state, NULL);
	hash_table_destroy(renderer->buffer_ht);

	srb_free_device_info(renderer->device_info);
	srb_free_context(&renderer->srb);
	free(renderer);
}

int
ice_renderer_init(struct weston_compositor *ec)
{
	struct ice_renderer *renderer;
	gdl_ret_t rc;

	renderer = malloc(sizeof *renderer);
	if (renderer == NULL)
		return -1;

	rc = srb_alloc_context(0, &renderer->srb);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to allocate srb context: %s\n",
			   gdl_get_error_string(rc));
		free(renderer);
		return -1;
	}

	rc = srb_get_device_info(0, &renderer->device_info);
	if (rc != GDL_SUCCESS) {
		weston_log("failed to get srb device info: %s\n",
			   gdl_get_error_string(rc));
		srb_free_context(&renderer->srb);
		free(renderer);
		return -1;
	}

	renderer->buffer_ht = hash_table_create();

	renderer->base.read_pixels = ice_renderer_read_pixels;
	renderer->base.repaint_output = ice_renderer_repaint_output;
	renderer->base.flush_damage = ice_renderer_flush_damage;
	renderer->base.attach = ice_renderer_attach;
	renderer->base.surface_set_color = ice_renderer_surface_set_color;
	renderer->base.destroy = ice_renderer_destroy;

	ec->renderer = &renderer->base;

	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_RGB565);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_ARGB4444);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_ARGB1555);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_ABGR8888);

	wl_signal_init(&renderer->destroy_signal);

	weston_log("IntelCE SRB renderer using device %s\n",
		   renderer->device_info->name);

	return 0;
}
