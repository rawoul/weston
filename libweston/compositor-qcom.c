#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <endian.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/input.h>
#include <linux/fb.h>
#include <linux/msm_ion.h>
#include <linux/msm_mdp.h>
#include <linux/msm_mdp_ext.h>
#include <linux/videodev2.h>

#include <drm_fourcc.h>

#include "compositor-qcom.h"
#include "compositor.h"
#include "lh-input.h"
#include "pixman-renderer.h"
#include "presentation-time-server-protocol.h"
#include "linux-dmabuf.h"
#include "shared/helpers.h"

//#define DEBUG

#ifdef DEBUG
# define dbg(fmt, ...) weston_log(fmt, ##__VA_ARGS__)
# define dbg_continue(fmt, ...) weston_log_continue(STAMP_SPACE fmt, \
						    ##__VA_ARGS__)
#else
# define dbg(fmt, ...) ({})
# define dbg_continue(fmt, ...) ({})
#endif

#define MDP_INVALID_FORMAT MDP_IMGTYPE_LIMIT2

#define SYSFS_MDP_DIR	"/sys/devices/soc/900000.qcom,mdss_mdp"

struct qcom_hwpipe {
	uint32_t index;
	enum mdp_overlay_pipe_type type;
};

struct qcom_hwinfo {
	uint32_t hw_version;
	uint32_t hw_revision;
	uint32_t n_blending_stages;
	uint32_t max_cursor_size;
	uint32_t max_scale_up;
	uint32_t max_scale_down;
	uint32_t max_pipe_width;
	uint32_t max_mixer_width;
	uint32_t has_ubwc : 1;
	uint32_t has_decimation : 1;
	uint32_t has_src_split : 1;
	uint32_t has_rotator_downscale : 1;
};

struct qcom_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;
	struct input_lh input;
	uint32_t output_transform;

	int ion_fd;

	struct qcom_hwinfo hwinfo;
	struct wl_list plane_list;
};

struct qcom_fb {
	int fd;
	int offset;
	int width;
	int height;
	int stride;
	int format;
	void *data;
	bool is_client_buffer;
	struct weston_buffer_reference buffer_ref;
};

struct qcom_screeninfo {
	unsigned int x_resolution; /* pixels, visible area */
	unsigned int y_resolution; /* pixels, visible area */
	unsigned int width_mm; /* visible screen width in mm */
	unsigned int height_mm; /* visible screen height in mm */
	unsigned int bits_per_pixel;

	size_t buffer_length; /* length of frame buffer memory in bytes */
	size_t line_length; /* length of a line in bytes */
	char id[16]; /* screen identifier */

	pixman_format_code_t pixel_format; /* frame buffer pixel format */
	unsigned int refresh_rate; /* Hertz */
};

struct qcom_output {
	struct qcom_backend *backend;
	struct weston_output base;

	struct weston_mode mode;

	int vsync_fd;
	struct wl_event_source *vsync_event;

	/* Frame buffer details. */
	char *device;
	int fd;
	struct qcom_screeninfo fb_info;
	struct qcom_fb *fb[2];
	pixman_image_t *image[2];
	int current_fb;
	int zorder;

	pixman_region32_t previous_damage;
};

struct qcom_plane {
	struct weston_plane base;
	struct wl_list link;
	uint32_t index;
	enum mdp_overlay_pipe_type type;
	struct qcom_fb *current;
	struct qcom_fb *next;
	struct mdp_rect src;
	struct mdp_rect dst;
	uint8_t alpha;
	uint32_t format;
	enum mdss_mdp_blend_op blend_op;
	int zorder;
};

static int qcom_fbdev_vsync_on(struct qcom_output *output);
static int qcom_fbdev_vsync_off(struct qcom_output *output);

static void qcom_fb_destroy(struct qcom_fb *fb);
static struct qcom_fb *qcom_fb_get_from_dmabuf(struct qcom_backend *backend,
		const struct linux_dmabuf_buffer *dmabuf);
static void qcom_fb_set_buffer(struct qcom_fb *fb,
			       struct weston_buffer *buffer);

static inline struct qcom_output *
qcom_output(struct weston_output *base)
{
	return container_of(base, struct qcom_output, base);
}

static inline struct qcom_backend *
qcom_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct qcom_backend, base);
}

static void
qcom_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts,
				   WP_PRESENTATION_FEEDBACK_INVALID);
}

static struct qcom_plane *
find_plane_type(struct qcom_backend *backend, enum mdp_overlay_pipe_type type)
{
	struct qcom_plane *plane;

	wl_list_for_each(plane, &backend->plane_list, link) {
		if (plane->type == type && !plane->next)
			return plane;
	}

	return NULL;
}

static int
drm_fourcc_to_mdp(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGB565:
		return MDP_RGB_565;
	case DRM_FORMAT_BGR565:
		return MDP_BGR_565;
	case DRM_FORMAT_RGB888:
		return MDP_RGB_888;
	case DRM_FORMAT_BGR888:
		return MDP_BGR_888;
	case DRM_FORMAT_XRGB8888:
		return MDP_XRGB_8888;
	case DRM_FORMAT_RGBX8888:
		return MDP_RGBX_8888;
	case DRM_FORMAT_ARGB8888:
		return MDP_ARGB_8888;
	case DRM_FORMAT_RGBA8888:
		return MDP_RGBA_8888;
	case DRM_FORMAT_BGRA8888:
		return MDP_BGRA_8888;
	case DRM_FORMAT_ARGB1555:
		return MDP_ARGB_1555;
	case DRM_FORMAT_RGBA5551:
		return MDP_RGBA_5551;
	case DRM_FORMAT_ARGB4444:
		return MDP_ARGB_4444;
	case DRM_FORMAT_RGBA4444:
		return MDP_RGBA_4444;
	/* Video proprietary formats */
	case V4L2_PIX_FMT_NV12:
		return MDP_Y_CBCR_H2V2_VENUS;
	case V4L2_PIX_FMT_NV12_UBWC:
		return MDP_Y_CBCR_H2V2_UBWC;
	case V4L2_PIX_FMT_RGBA8888_UBWC:
		return MDP_RGBA_8888_UBWC;
	default:
		return -1;
	}
}

static bool
mdp_format_has_ubwc(uint32_t format)
{
	switch (format) {
	case MDP_RGB_565_UBWC:
	case MDP_RGBA_8888_UBWC:
	case MDP_Y_CBCR_H2V2_UBWC:
	case MDP_RGBX_8888_UBWC:
		return true;
	default:
		return false;
	}
}

static bool
mdp_format_has_alpha(uint32_t format)
{
	switch (format) {
	case MDP_ARGB_8888:
	case MDP_RGBA_8888:
	case MDP_BGRA_8888:
	case MDP_RGBA_8888_TILE:
	case MDP_ARGB_8888_TILE:
	case MDP_ABGR_8888_TILE:
	case MDP_BGRA_8888_TILE:
	case MDP_ARGB_1555:
	case MDP_RGBA_5551:
	case MDP_ARGB_4444:
	case MDP_RGBA_4444:
	case MDP_RGBA_8888_UBWC:
		return true;
	case MDP_RGB_565:
	case MDP_XRGB_8888:
	case MDP_Y_CBCR_H2V2:
	case MDP_Y_CBCR_H2V2_ADRENO:
	case MDP_RGB_888:
	case MDP_Y_CRCB_H2V2:
	case MDP_YCRYCB_H2V1:
	case MDP_CBYCRY_H2V1:
	case MDP_Y_CRCB_H2V1:
	case MDP_Y_CBCR_H2V1:
	case MDP_Y_CRCB_H1V2:
	case MDP_Y_CBCR_H1V2:
	case MDP_RGBX_8888:
	case MDP_Y_CRCB_H2V2_TILE:
	case MDP_Y_CBCR_H2V2_TILE:
	case MDP_Y_CR_CB_H2V2:
	case MDP_Y_CR_CB_GH2V2:
	case MDP_Y_CB_CR_H2V2:
	case MDP_Y_CRCB_H1V1:
	case MDP_Y_CBCR_H1V1:
	case MDP_YCRCB_H1V1:
	case MDP_YCBCR_H1V1:
	case MDP_BGR_565:
	case MDP_BGR_888:
	case MDP_Y_CBCR_H2V2_VENUS:
	case MDP_BGRX_8888:
	case MDP_RGBX_8888_TILE:
	case MDP_XRGB_8888_TILE:
	case MDP_XBGR_8888_TILE:
	case MDP_BGRX_8888_TILE:
	case MDP_YCBYCR_H2V1:
	case MDP_RGB_565_TILE:
	case MDP_BGR_565_TILE:
	case MDP_RGB_565_UBWC:
	case MDP_Y_CBCR_H2V2_UBWC:
	case MDP_RGBX_8888_UBWC:
	case MDP_Y_CRCB_H2V2_VENUS:
	case MDP_IMGTYPE_LIMIT:
	case MDP_RGB_BORDERFILL:
	case MDP_FB_FORMAT:
	case MDP_IMGTYPE_LIMIT2:
		break;
	}

	return false;
}

static uint32_t
mdp_format_without_alpha(uint32_t format)
{
	switch (format) {
	case MDP_ARGB_8888:
		return MDP_XRGB_8888;
	case MDP_RGBA_8888:
		return MDP_RGBX_8888;
	case MDP_BGRA_8888:
		return MDP_BGRX_8888;
	case MDP_RGBA_8888_TILE:
		return MDP_RGBX_8888_TILE;
	case MDP_ARGB_8888_TILE:
		return MDP_XRGB_8888_TILE;
	case MDP_ABGR_8888_TILE:
		return MDP_XBGR_8888_TILE;
	case MDP_BGRA_8888_TILE:
		return MDP_BGRX_8888_TILE;
	case MDP_RGBA_8888_UBWC:
		return MDP_RGBX_8888_UBWC;
	default:
		return MDP_INVALID_FORMAT;
	}
}

static uint8_t
calculate_decimation(struct qcom_backend *backend, uint32_t src, uint32_t dst)
{
	uint8_t decimation = 0;

	while (src > backend->hwinfo.max_pipe_width * (decimation + 1))
		decimation++;

	dst *= backend->hwinfo.max_scale_down;
	while (dst * (decimation + 1) < src)
		decimation++;

	return decimation;
}

static void
fill_layer_config(struct qcom_backend *backend,
		  const struct qcom_plane *plane,
		  struct mdp_input_layer *layer)
{
	struct qcom_fb *fb = plane->next;

	layer->pipe_ndx = plane->index;
	layer->alpha = plane->alpha;
	layer->color_space = MDP_CSC_ITU_R_709;
	layer->src_rect = plane->src;
	layer->dst_rect = plane->dst;
	layer->horz_deci =
		calculate_decimation(backend, plane->src.w, plane->dst.w);
	layer->vert_deci =
		calculate_decimation(backend, plane->src.h, plane->dst.h);
	layer->z_order = plane->zorder;
	layer->blend_op = plane->blend_op;
	// HACK for UBWC...
	layer->buffer.width = plane->format == MDP_Y_CBCR_H2V2_UBWC ?
		fb->stride : fb->width;
	layer->buffer.height = fb->height;
	layer->buffer.format = plane->format;
	layer->buffer.plane_count = 1;
	layer->buffer.planes[0].fd = fb->fd;
	layer->buffer.planes[0].offset = fb->offset;
	layer->buffer.planes[0].stride = fb->stride;
	layer->buffer.comp_ratio.numer = 1;
	layer->buffer.comp_ratio.denom = 1;
	layer->buffer.fence = -1;

	dbg("config layer=%x type=%x z=%d a=%d "
	    "buffer=%u(%u)x%u "
	    "src=%ux%u+%u+%u "
	    "dst=%ux%u+%u+%u "
	    "decimate=%ux%u\n",
	    layer->pipe_ndx, plane->type,
	    layer->z_order,
	    layer->alpha,
	    layer->buffer.width,
	    layer->buffer.planes[0].stride, layer->buffer.height,
	    layer->src_rect.w, layer->src_rect.h,
	    layer->src_rect.x, layer->src_rect.y,
	    layer->dst_rect.w, layer->dst_rect.h,
	    layer->dst_rect.x, layer->dst_rect.y,
	    layer->horz_deci, layer->vert_deci);
}

static int
qcom_output_commit(struct qcom_output *output)
{
	struct qcom_backend *backend = output->backend;
	struct qcom_plane *plane;
	struct mdp_input_layer in_layers[8];
	struct mdp_layer_commit commit;
	int num_layers = 0;

	memset(in_layers, 0, sizeof (in_layers));

	wl_list_for_each(plane, &backend->plane_list, link) {
		if (!plane->next)
			continue;

		fill_layer_config(backend, plane, &in_layers[num_layers]);
		num_layers++;
	}

	plane = find_plane_type(backend, PIPE_TYPE_RGB);
	if (!plane) {
		weston_log("no available plane for framebuffer\n");
	} else {
		struct qcom_fb *fb;

		fb = output->fb[output->current_fb];

		plane->next = fb;
		plane->alpha = 255;
		plane->src.x = 0;
		plane->src.y = 0;
		plane->src.w = fb->width;
		plane->src.h = fb->height;
		plane->dst = plane->src;
		plane->zorder = output->zorder;
		plane->format = fb->format;
		plane->blend_op = BLEND_OP_OPAQUE;
		fill_layer_config(backend, plane, &in_layers[num_layers]);
		num_layers++;
	}

	memset(&commit, 0, sizeof (commit));
	commit.version = MDP_COMMIT_VERSION_1_0;
	commit.commit_v1.input_layers = in_layers;
	commit.commit_v1.input_layer_cnt = num_layers;
	commit.commit_v1.output_layer = NULL;
	commit.commit_v1.release_fence = -1;
	commit.commit_v1.retire_fence = -1;

	if (ioctl(output->fd, MSMFB_ATOMIC_COMMIT, &commit) < 0) {
		weston_log("failed to commit: %m\n");
		return -1;
	}

	if (commit.commit_v1.release_fence != -1)
		close(commit.commit_v1.release_fence);

	if (commit.commit_v1.retire_fence != -1)
		close(commit.commit_v1.retire_fence);

	if (output->vsync_fd < 0 && qcom_fbdev_vsync_on(output) < 0)
		return -1;

	return 0;
}

static int
qcom_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct qcom_output *output = qcom_output(base);
	struct qcom_backend *backend = output->backend;
	struct weston_compositor *ec = backend->compositor;

	if (pixman_region32_not_empty(damage)) {
		pixman_region32_t total_damage;

		output->current_fb ^= 1;

		pixman_region32_init(&total_damage);
		pixman_region32_union(&total_damage, damage,
				      &output->previous_damage);
		pixman_region32_copy(&output->previous_damage, damage);

		pixman_renderer_output_set_buffer(base,
				output->image[output->current_fb]);
		ec->renderer->repaint_output(base, &total_damage);

		pixman_region32_fini(&total_damage);

		pixman_region32_subtract(&ec->primary_plane.damage,
					 &ec->primary_plane.damage, damage);
	}

	if (qcom_output_commit(output) < 0)
		return -1;

	/* reset output plane layout */
	output->zorder = backend->hwinfo.n_blending_stages - 1;

	return 0;
}

static int
finish_frame_handler(int fd, uint32_t mask, void *data)
{
	struct qcom_output *output = data;
	char buf[64];
	int ret;

	if (!(mask & WL_EVENT_URGENT))
		return 0;

	ret = pread(fd, buf, sizeof (buf), 0);
	if (ret < 0) {
		weston_log("failed to read vsync event: %m\n");
		return 0;
	}

	if (!strncmp(buf, "VSYNC=", 6)) {
		struct timespec ts;
		int64_t timestamp = strtoull(buf + 6, NULL, 0);
		struct qcom_plane *plane;

		ts.tv_sec = timestamp / 1000000000;
		ts.tv_nsec = timestamp % 1000000000;

		wl_list_for_each(plane, &output->backend->plane_list, link) {
			if (plane->next) {
				plane->current = plane->next;
				plane->next = NULL;
			}
		}

		weston_output_finish_frame(&output->base, &ts,
				WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION |
				WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
	}

	return 0;
}

static struct weston_plane *
qcom_output_prepare_overlay_view(struct qcom_output *output,
				 struct weston_view *view)
{
	struct qcom_backend *b = output->backend;
	struct qcom_plane *plane;
	struct weston_surface *surface;
	struct weston_buffer_viewport *viewport;
	struct wl_resource *buffer_resource;
	struct linux_dmabuf_buffer *dmabuf;
	struct qcom_fb *fb = NULL;
	float sx1, sy1, sx2, sy2;

	surface = view->surface;
	viewport = &view->surface->buffer_viewport;

	if (viewport->buffer.transform != output->base.transform)
		return NULL;

	if (view->output_mask != (1u << output->base.id))
		return NULL;

	if (surface->buffer_ref.buffer == NULL)
		return NULL;

	buffer_resource = surface->buffer_ref.buffer->resource;

	dbg("try to assign view to overlay\n");

	// check view transform is supported
	if (view->transform.enabled &&
	    view->transform.matrix.type >= WESTON_MATRIX_TRANSFORM_ROTATE) {
		dbg_continue("transform not supported\n");
		return NULL;
	}

	// create fb for buffer
	if ((dmabuf = linux_dmabuf_buffer_get(buffer_resource))) {
		plane = find_plane_type(b, PIPE_TYPE_VIG);
		if (!plane) {
			dbg_continue(" -> no vig plane available\n");
			return NULL;
		}

		fb = qcom_fb_get_from_dmabuf(b, dmabuf);
	}

	if (!fb) {
		dbg_continue(" -> unhandled buffer type\n");
		return NULL;
	}

	qcom_fb_set_buffer(fb, surface->buffer_ref.buffer);

	// start computing pipe configuration
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

	dbg_continue(" . dst rect %gx%g%+g%+g\n",
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
	plane->dst.x = nearbyintf(p1.f[0]);
	plane->dst.y = nearbyintf(p1.f[1]);
	plane->dst.w = nearbyintf(p2.f[0] - p1.f[0]);
	plane->dst.h = nearbyintf(p2.f[1] - p1.f[1]);

	dbg_continue(" . clipped to %ux%u+%u+%u\n",
		     plane->dst.w, plane->dst.h, plane->dst.x, plane->dst.y);

	// check clipped size is supported
	if (plane->dst.w > b->hwinfo.max_pipe_width) {
		dbg_continue(" -> surface size not supported by hw\n");
		goto reject;
	}

	// check opaque region when using a pixel format with alpha
	if (mdp_format_has_alpha(fb->format) &&
	    pixman_region32_not_empty(&surface->opaque)) {
		pixman_region32_t non_opaque;

		pixman_region32_init_rect(&non_opaque, 0, 0,
					  surface->width, surface->height);
		pixman_region32_subtract(&non_opaque, &non_opaque,
					 &surface->opaque);

		if (pixman_region32_not_empty(&non_opaque)) {
			pixman_region32_fini(&non_opaque);
			dbg_continue(" -> argb surface not totally opaque\n");
			goto reject;
		}

		pixman_region32_fini(&non_opaque);

#if 0
		uint32_t xformat;

		xformat = mdp_format_without_alpha(fb->format);
		if (xformat == MDP_INVALID_FORMAT) {
			dbg_continue(" ->  opaque surface with alpha format\n");
			return NULL;
		}

		plane->format = xformat;
#else
		plane->format = fb->format;
#endif
		plane->blend_op = BLEND_OP_OPAQUE;
	} else {
		plane->format = fb->format;
		plane->blend_op = BLEND_OP_NOT_DEFINED;
	}

	// convert output coordinates back to buffer coordinates
	weston_surface_to_buffer_float(surface, sx1, sy1, &sx1, &sy1);
	weston_surface_to_buffer_float(surface, sx2, sy2, &sx2, &sy2);

	plane->src.x = nearbyintf(sx1);
	plane->src.y = nearbyintf(sy1);
	plane->src.w = nearbyintf(sx2 - sx1);
	plane->src.h = nearbyintf(sy2 - sy1);

	if (plane->src.x & 1)
		plane->src.x--;
	if (plane->src.y & 1)
		plane->src.y--;
	if (plane->src.w & 1)
		plane->src.w--;
	if (plane->src.h & 1)
		plane->src.h--;

	dbg_continue(" . src rect: %ux%u+%u+%u\n",
		     plane->src.w, plane->src.h, plane->src.x, plane->src.y);

	// check scaler configuration
	if (plane->src.w != plane->dst.w || plane->src.h != plane->dst.h) {
		int max_scale_up = b->hwinfo.max_scale_up;
		int max_scale_down = b->hwinfo.max_scale_down;
		int max_decimation = 16;

		if (b->hwinfo.has_decimation &&
		    !mdp_format_has_ubwc(plane->format))
			max_scale_down *= max_decimation;

		if (plane->src.w * max_scale_up < plane->dst.w ||
		    plane->src.h * max_scale_up < plane->dst.h ||
		    plane->src.w > plane->dst.w * max_scale_down ||
		    plane->src.h > plane->dst.h * max_scale_down) {
			dbg_continue(" -> scaling factor not supported\n");
			goto reject;
		}
	}

	plane->alpha = roundf(weston_view_get_alpha(view) * 255.0f);
	plane->zorder = output->zorder--;
	plane->next = fb;

	return &plane->base;

offscreen:
	dbg_continue(" -> surface out of screen\n");
reject:
	qcom_fb_destroy(fb);
	return NULL;
}

static struct weston_plane *
qcom_output_assign_plane(struct qcom_output *output, struct weston_view *view,
			 pixman_region32_t *composited_region)
{
	struct weston_compositor *ec = output->base.compositor;
	struct weston_plane *plane;
	struct weston_plane *primary;
	struct weston_surface *surface;
	pixman_region32_t overlap;

	primary = &ec->primary_plane;

#if 0
	if (view->layer_link.layer == &backend->background_layer) {
		// dummy layer to track composited framebuffer damage
		return primary;
	}

	if (view->layer_link.layer == &ec->cursor_layer) {
		if ((plane = ice_output_assign_cursor_view(output, view)))
			return plane;
	}
#endif

	surface = view->surface;
	surface->keep_buffer = surface->buffer_ref.buffer &&
		!wl_shm_buffer_get(surface->buffer_ref.buffer->resource);

#ifdef DEBUG
	pixman_box32_t *extents =
		pixman_region32_extents(&view->transform.boundingbox);

	dbg("assign %dx%d%+d%+d view\n",
	    extents->x2 - extents->x1, extents->y2 - extents->y1,
	    extents->x1, extents->y1);
#endif

	pixman_region32_init(&overlap);
	pixman_region32_intersect(&overlap, composited_region,
				  &view->transform.boundingbox);

	plane = NULL;
	if (!pixman_region32_not_empty(&overlap))
		plane = qcom_output_prepare_overlay_view(output, view);
	if (plane != NULL) {
		dbg_continue(" -> assigned view to overlay\n");
	} else {
		dbg_continue(" -> assigned view to primary\n");
		plane = primary;
	}

	pixman_region32_fini(&overlap);

	if (plane == primary)
		pixman_region32_union(composited_region, composited_region,
				      &view->transform.boundingbox);

	if (plane == primary /*|| plane == &output->cursor_plane.base*/) {
		/* cursor plane involves a copy */
		view->psf_flags = 0;
	} else {
		/* all other planes scanout the client buffer directly */
		view->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
	}

	return plane;
}

static void
qcom_output_assign_planes(struct weston_output *base)
{
	struct qcom_output *output = qcom_output(base);
	struct weston_compositor *compositor = base->compositor;
	struct weston_plane *plane;
	struct weston_view *ev;
	pixman_region32_t composited_region;

	dbg("assign planes\n");

	pixman_region32_init(&composited_region);

	wl_list_for_each(ev, &compositor->view_list, link) {
		plane = qcom_output_assign_plane(output, ev,
						 &composited_region);
		weston_view_move_to_plane(ev, plane);
	}

	pixman_region32_fini(&composited_region);
}

static void
qcom_fb_destroy(struct qcom_fb *fb)
{
	if (fb->data != NULL) {
		if (munmap(fb->data, fb->stride * fb->height) < 0)
			weston_log("failed to unmap framebuffer: %m\n");
	}

	if (!fb->is_client_buffer && fb->fd != -1) {
		if (close(fb->fd) < 0)
			weston_log("failed to close fb: %m\n");
	}

	weston_buffer_reference(&fb->buffer_ref, NULL);

	free(fb);
}

static void
qcom_fb_set_buffer(struct qcom_fb *fb, struct weston_buffer *buffer)
{
	assert(fb->buffer_ref.buffer == NULL);
	weston_buffer_reference(&fb->buffer_ref, buffer);
	fb->is_client_buffer = true;
}

static struct qcom_fb *
qcom_fb_get_from_dmabuf(struct qcom_backend *backend,
			const struct linux_dmabuf_buffer *dmabuf)
{
	struct qcom_fb *fb;
	int format;

	format = drm_fourcc_to_mdp(dmabuf->attributes.format);
	if (format < 0) {
		dbg("unknown format %.*s\n", 4,
		    (const char *)&dmabuf->attributes.format);
		return NULL;
	}

	if (dmabuf->attributes.n_planes != 1) {
		dbg("cannot use dmabuf with multiple planes\n");
		return NULL;
	}

	fb = zalloc(sizeof *fb);
	if (!fb)
		return NULL;

	fb->fd = dmabuf->attributes.fd[0];
	fb->offset = dmabuf->attributes.offset[0];
	fb->width = dmabuf->attributes.width;
	fb->height = dmabuf->attributes.height;
	fb->format = format;
	fb->stride = dmabuf->attributes.stride[0];

	return fb;
}

static struct qcom_fb *
qcom_fb_create(struct qcom_backend *backend, int width, int height)
{
	struct qcom_fb *fb;
	struct ion_allocation_data ion_alloc;
	struct ion_fd_data ion_fd;
	struct ion_handle_data ion_handle;

	fb = zalloc(sizeof *fb);
	if (!fb)
		return NULL;

	fb->width = width;
	fb->height = height;
	fb->format = MDP_BGRA_8888;
	fb->stride = width * 4;
	fb->fd = -1;

	memset(&ion_alloc, 0, sizeof (ion_alloc));
	ion_alloc.len = fb->stride * height;
	ion_alloc.align = sysconf(_SC_PAGESIZE);
	ion_alloc.heap_id_mask = ION_HEAP(ION_SYSTEM_HEAP_ID);
	ion_alloc.flags = 0; //ION_FLAG_CACHED;

	if (ioctl(backend->ion_fd, ION_IOC_ALLOC, &ion_alloc) < 0) {
		weston_log("failed to allocate %dx%d ion buffer: %m\n",
			   width, height);
		goto fail;
	}

	memset(&ion_fd, 0, sizeof (ion_fd));
	ion_fd.handle = ion_alloc.handle;

	if (ioctl(backend->ion_fd, ION_IOC_MAP, &ion_fd) < 0) {
		weston_log("failed to map ion buffer: %m\n");
		goto fail;
	}

	fb->fd = ion_fd.fd;

	fb->data = mmap(0, ion_alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED,
			fb->fd, 0);
	if (fb->data == MAP_FAILED) {
		weston_log("failed to map ion buffer: %m\n");
		goto fail;
	}

ion_free:
	memset(&ion_handle, 0, sizeof (ion_handle));
	ion_handle.handle = ion_alloc.handle;

	if (ioctl(backend->ion_fd, ION_IOC_FREE, &ion_handle) < 0)
		weston_log("failed to release ion buffer: %m\n");

	return fb;

fail:
	qcom_fb_destroy(fb);
	fb = NULL;
	goto ion_free;
}

static pixman_format_code_t
calculate_pixman_format(struct fb_var_screeninfo *vinfo,
                        struct fb_fix_screeninfo *finfo)
{
	/* Calculate the pixman format supported by the frame buffer from the
	 * buffer's metadata. Return 0 if no known pixman format is supported
	 * (since this has depth 0 it's guaranteed to not conflict with any
	 * actual pixman format).
	 *
	 * Documentation on the vinfo and finfo structures:
	 *    http://www.mjmwired.net/kernel/Documentation/fb/api.txt
	 *
	 * TODO: Try a bit harder to support other formats, including setting
	 * the preferred format in the hardware. */
	int type;

	weston_log("Calculating pixman format from:\n"
	           STAMP_SPACE " - type: %i (aux: %i)\n"
	           STAMP_SPACE " - visual: %i\n"
	           STAMP_SPACE " - bpp: %i (grayscale: %i)\n"
	           STAMP_SPACE " - red: offset: %i, length: %i, MSB: %i\n"
	           STAMP_SPACE " - green: offset: %i, length: %i, MSB: %i\n"
	           STAMP_SPACE " - blue: offset: %i, length: %i, MSB: %i\n"
	           STAMP_SPACE " - transp: offset: %i, length: %i, MSB: %i\n",
	           finfo->type, finfo->type_aux, finfo->visual,
	           vinfo->bits_per_pixel, vinfo->grayscale,
	           vinfo->red.offset, vinfo->red.length, vinfo->red.msb_right,
	           vinfo->green.offset, vinfo->green.length,
	           vinfo->green.msb_right,
	           vinfo->blue.offset, vinfo->blue.length,
	           vinfo->blue.msb_right,
	           vinfo->transp.offset, vinfo->transp.length,
	           vinfo->transp.msb_right);

	/* We only handle packed formats at the moment. */
	if (finfo->type != FB_TYPE_PACKED_PIXELS)
		return 0;

	/* We only handle true-colour frame buffers at the moment. */
	switch (finfo->visual) {
		case FB_VISUAL_TRUECOLOR:
		case FB_VISUAL_DIRECTCOLOR:
			if (vinfo->grayscale != 0)
				return 0;
		break;
		default:
			return 0;
	}

	/* We only support formats with MSBs on the left. */
	if (vinfo->red.msb_right != 0 || vinfo->green.msb_right != 0 ||
	    vinfo->blue.msb_right != 0)
		return 0;

	/* Work out the format type from the offsets. We only support RGBA and
	 * ARGB at the moment. */
	type = PIXMAN_TYPE_OTHER;

	if ((vinfo->transp.offset >= vinfo->red.offset ||
	     vinfo->transp.length == 0) &&
	    vinfo->red.offset >= vinfo->green.offset &&
	    vinfo->green.offset >= vinfo->blue.offset)
		type = PIXMAN_TYPE_ARGB;
	else if (vinfo->red.offset >= vinfo->green.offset &&
	         vinfo->green.offset >= vinfo->blue.offset &&
	         vinfo->blue.offset >= vinfo->transp.offset)
		type = PIXMAN_TYPE_RGBA;
	else if (vinfo->transp.offset >= vinfo->blue.offset &&
		 vinfo->blue.offset >= vinfo->green.offset &&
		 vinfo->green.offset >= vinfo->red.offset)
		type = PIXMAN_TYPE_ABGR;
	else if (vinfo->blue.offset >= vinfo->green.offset &&
		 vinfo->green.offset >= vinfo->red.offset &&
		 vinfo->red.offset >= vinfo->transp.offset)
		type = PIXMAN_TYPE_BGRA;

	if (type == PIXMAN_TYPE_OTHER)
		return 0;

	/* Build the format. */
	return PIXMAN_FORMAT(vinfo->bits_per_pixel, type,
	                     vinfo->transp.length,
	                     vinfo->red.length,
	                     vinfo->green.length,
	                     vinfo->blue.length);
}

static int
qcom_query_refresh_rate(int fd)
{
	struct msmfb_metadata metadata;

	memset(&metadata, 0, sizeof(metadata));
	metadata.op = metadata_op_frame_rate;

	if (ioctl(fd, MSMFB_METADATA_GET, &metadata) < 0) {
		weston_log("failed to get framebuffer frame rate: %m\n");
		return -1;
	}

	return metadata.data.panel_frame_rate;
}

static int
qcom_query_screen_info(struct qcom_output *output, int fd,
		       struct qcom_screeninfo *info)
{
	struct fb_var_screeninfo varinfo;
	struct fb_fix_screeninfo fixinfo;

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	info->x_resolution = varinfo.xres;
	info->y_resolution = varinfo.yres;

	if (varinfo.width <= 0 || varinfo.height <= 0) {
		info->width_mm = roundf(varinfo.xres * 25.4f / 96.0f);
		info->height_mm = roundf(varinfo.yres * 25.4f / 96.0f);
	} else {
		info->width_mm = varinfo.width;
		info->height_mm = varinfo.height;
	}

	info->bits_per_pixel = varinfo.bits_per_pixel;
	info->buffer_length = fixinfo.smem_len;
	info->line_length = fixinfo.line_length;
	strncpy(info->id, fixinfo.id, sizeof(info->id));
	info->id[sizeof(info->id)-1] = '\0';

	info->pixel_format = calculate_pixman_format(&varinfo, &fixinfo);
	info->refresh_rate = qcom_query_refresh_rate(fd);

	if (info->pixel_format == 0) {
		weston_log("frame buffer uses an unsupported format\n");
		return -1;
	}

	return 1;
}

static void
qcom_fbdev_destroy(struct qcom_output *output)
{
	weston_log("destroying fbdev frame buffer.\n");

	qcom_fbdev_vsync_off(output);

	if (close(output->fd) < 0)
		weston_log("failed to close frame buffer: %m\n");

	output->fd = -1;
}

static int
qcom_fbdev_open(struct qcom_output *output, const char *fb_dev,
		struct qcom_screeninfo *screen_info)
{
	int fd;

	weston_log("opening fbdev frame buffer\n");

	fd = open(fb_dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		weston_log("failed to open frame buffer device ‘%s’: %m\n",
			   fb_dev);
		return -1;
	}

	if (qcom_query_screen_info(output, fd, screen_info) < 0) {
		weston_log("failed to get frame buffer info: %m\n");
		close(fd);
		return -1;
	}

	output->fd = fd;

	return 0;
}

static int
qcom_fbdev_vsync_on(struct qcom_output *output)
{
	struct wl_event_loop *loop;
	int enable = 1;
	int fd;

	if (output->vsync_fd != -1)
		return 0;

	if (ioctl(output->fd, MSMFB_OVERLAY_VSYNC_CTRL, &enable) < 0) {
		weston_log("failed to enable vsync ctrl: %m\n");
		return -1;
	}

	fd = open("/sys/class/graphics/fb0/vsync_event", O_RDONLY);
	if (fd < 0) {
		weston_log("failed to open vsync event file: %m");
		return -1;
	}

	output->vsync_fd = fd;

	loop = wl_display_get_event_loop(output->base.compositor->wl_display);
	output->vsync_event =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_URGENT,
				     finish_frame_handler, output);

	return 0;
}

static int
qcom_fbdev_vsync_off(struct qcom_output *output)
{
	int enable = 0;

	if (output->vsync_fd == -1)
		return 0;

	if (close(output->vsync_fd) < 0)
		weston_log("failed to close vsync fd: %m\n");

	output->vsync_fd = -1;

	if (output->vsync_event != NULL) {
		wl_event_source_remove(output->vsync_event);
		output->vsync_event = NULL;
	}

	if (ioctl(output->fd, MSMFB_OVERLAY_VSYNC_CTRL, &enable) < 0) {
		weston_log("failed to disable vsync ctrl: %m\n");
		return -1;
	}

	return 0;
}

static int
qcom_output_init_pixman(struct qcom_output *output)
{
	int w = output->base.current_mode->width;
	int h = output->base.current_mode->height;

	for (unsigned i = 0; i < ARRAY_LENGTH(output->fb); i++) {
		output->fb[i] = qcom_fb_create(output->backend, w, h);
		if (!output->fb[i])
			goto fail;

		output->image[i] =
			pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h,
						 output->fb[i]->data,
						 output->fb[i]->stride);
		if (output->image[i] == NULL) {
			weston_log("failed to create image for fb\n");
			goto fail;
		}
	}

	if (pixman_renderer_output_create(&output->base) < 0)
		goto fail;

	pixman_region32_init_rect(&output->previous_damage,
				  output->base.x, output->base.y,
				  output->base.width, output->base.height);

	return 0;

fail:
	for (unsigned i = 0; i < ARRAY_LENGTH(output->fb); i++) {
		if (output->fb[i])
			qcom_fb_destroy(output->fb[i]);
		if (output->image[i])
			pixman_image_unref(output->image[i]);
		output->fb[i] = NULL;
		output->image[i] = NULL;
	}

	return -1;
}

static void
qcom_output_destroy(struct weston_output *base)
{
	struct qcom_output *output = qcom_output(base);

	weston_log("destroying fbdev output\n");

	qcom_fbdev_destroy(output);

	pixman_region32_fini(&output->previous_damage);

	if (base->renderer_state != NULL)
		pixman_renderer_output_destroy(base);

	for (unsigned i = 0; i < ARRAY_LENGTH(output->fb); i++) {
		if (output->fb[i] != NULL)
			qcom_fb_destroy(output->fb[i]);
		if (output->image[i] != NULL)
			pixman_image_unref(output->image[i]);
	}

	weston_output_destroy(&output->base);

	free(output->device);
	free(output);
}

static int
qcom_output_create(struct qcom_backend *backend, const char *device)
{
	struct qcom_output *output;

	weston_log("creating fbdev output\n");

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->backend = backend;
	output->device = strdup(device);
	output->vsync_fd = -1;

	if (qcom_fbdev_open(output, device, &output->fb_info) < 0)
		goto err_free;

	output->base.start_repaint_loop = qcom_output_start_repaint_loop;
	output->base.assign_planes = qcom_output_assign_planes;
	output->base.repaint = qcom_output_repaint;
	output->base.destroy = qcom_output_destroy;
	//output->base.disable_planes = 1;

	/* only one static mode in list */
	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = output->fb_info.x_resolution;
	output->mode.height = output->fb_info.y_resolution;
	output->mode.refresh = output->fb_info.refresh_rate;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	output->base.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
	output->base.make = "Freebox";
	output->base.model = output->fb_info.id;
	output->base.name = strdup("fbdev");
	output->zorder = backend->hwinfo.n_blending_stages - 1;

	weston_output_init(&output->base, backend->compositor,
	                   0, 0, 0, 0, backend->output_transform, 1);

	if (qcom_output_init_pixman(output) < 0)
		goto err_output;

	weston_compositor_add_output(backend->compositor, &output->base);

	weston_log("fbdev output %d×%d@%dHz\n",
		   output->mode.width, output->mode.height,
		   output->mode.refresh / 1000);

	return 0;

err_output:
	weston_output_destroy(&output->base);

err_free:
	qcom_fbdev_destroy(output);
	free(output->device);
	free(output);
	return -1;
}

static void
qcom_plane_destroy(struct qcom_plane *plane)
{
	wl_list_remove(&plane->link);
	weston_plane_release(&plane->base);
	free(plane);
}

static struct qcom_plane *
qcom_plane_create(struct qcom_backend *backend, const struct qcom_hwpipe *pipe)
{
	struct qcom_plane *plane;

	plane = zalloc(sizeof (*plane));
	if (!plane)
		return NULL;

	weston_plane_init(&plane->base, backend->compositor, 0, 0);

	plane->type = pipe->type;
	plane->index = pipe->index;

	wl_list_insert(&backend->plane_list, &plane->link);

	return plane;
}

static int
parse_pipe_type(const char *s)
{
	if (!strcmp(s, "rgb"))
		return PIPE_TYPE_RGB;
	if (!strcmp(s, "vig"))
		return PIPE_TYPE_VIG;
	if (!strcmp(s, "dma"))
		return PIPE_TYPE_DMA;
	if (!strcmp(s, "cursor"))
		return PIPE_TYPE_CURSOR;
	return -1;
}

static int
parse_mdp_pipe(char *line, struct qcom_hwpipe *pipe)
{
	char *s, *token;
	int type = -1;
	int index = -1;

	while ((token = strtok_r(line, " ", &s)) != NULL) {
		char *value;

		line = NULL;

		value = strchr(token, ':');
		if (!value)
			continue;

		*value++ = '\0';

		if (!strcmp(token, "pipe_type"))
			type = parse_pipe_type(value);
		else if (!strcmp(token, "pipe_ndx"))
			index = atoi(value);
	}

	if (type < 0 || index < 0)
		return -1;

	pipe->type = type;
	pipe->index = index;

	dbg("add pipe %x type %x\n", index, type);

	return 0;
}

static uint32_t
parse_mdp_features(char *features, struct qcom_hwinfo *hwinfo)
{
	char *s, *token;
	uint32_t flags = 0;

	s = NULL;

	while ((token = strtok_r(features, " ", &s)) != NULL) {
		features = NULL;

		if (!strcmp(token, "ubwc"))
			hwinfo->has_ubwc = 1;
		else if (!strcmp(token, "decimation"))
			hwinfo->has_decimation = 1;
		else if (!strcmp(token, "src_split"))
			hwinfo->has_src_split = 1;
		else if (!strcmp(token, "rotator_downscale"))
			hwinfo->has_rotator_downscale = 1;
	}

	return flags;
}

static void
parse_mdp_caps(char *line, struct qcom_hwinfo *hwinfo)
{
	char *key = line;
	char *value = strchr(line, '=');

	if (!value)
		return;

	*value++ = '\0';

	if (!strcmp(key, "mdp_version")) {
		hwinfo->hw_version = atoi(value);
	} else if (!strcmp(key, "hw_rev")) {
		hwinfo->hw_revision = atoi(value);
	} else if (!strcmp(key, "blending_stages")) {
		hwinfo->n_blending_stages = atoi(value);
	} else if (!strcmp(key, "max_cursor_size")) {
		hwinfo->max_cursor_size = atoi(value);
	} else if (!strcmp(key, "max_upscale_ratio")) {
		hwinfo->max_scale_up = atoi(value);
	} else if (!strcmp(key, "max_downscale_ratio")) {
		hwinfo->max_scale_down = atoi(value);
	} else if (!strcmp(key, "max_pipe_width")) {
		hwinfo->max_pipe_width = atoi(value);
	} else if (!strcmp(key, "max_mixer_width")) {
		hwinfo->max_mixer_width = atoi(value);
	} else if (!strcmp(key, "features")) {
		parse_mdp_features(value, hwinfo);
	}
}

static int
qcom_init_hw_info(struct qcom_backend *backend)
{
	const char *caps_path = SYSFS_MDP_DIR "/caps";
	char line[128];
	FILE *f;

	f = fopen(caps_path, "r");
	if (f == NULL) {
		weston_log("failed to enumerate MDP capabilities at %s: %m\n",
			   caps_path);
		return -1;
	}

	while (fgets(line, sizeof (line), f)) {
		int len = strlen(line);

		if (line[len - 1] != '\n') {
			weston_log("line too long in MDP caps\n");
			continue;
		}

		line[len - 1] = '\0';

		if (!strncmp(line, "pipe_num:", 9)) {
			struct qcom_hwpipe pipe;

			if (parse_mdp_pipe(line, &pipe) < 0)
				continue;

			qcom_plane_create(backend, &pipe);
		} else {
			parse_mdp_caps(line, &backend->hwinfo);
		}
	}

	fclose(f);

	return 0;
}

static void
qcom_restore(struct weston_compositor *compositor)
{
}

static void
qcom_destroy(struct weston_compositor *compositor)
{
	struct qcom_backend *b = (struct qcom_backend *) compositor->backend;
	struct qcom_plane *plane, *next;

	wl_list_for_each_safe(plane, next, &b->plane_list, link)
		qcom_plane_destroy(plane);

	input_lh_shutdown(&b->input);
	weston_compositor_shutdown(compositor);

	if (close(b->ion_fd) < 0)
		weston_log("failed to close ion device: %m\n");

	free(b->pipes);
	free(b);
}

static void
debug_binding(struct weston_keyboard *keyboard, uint32_t time, uint32_t key,
	      void *data)
{
	struct qcom_backend *b = data;
	struct weston_output *output;

	switch (key) {
	case KEY_C:
		wl_list_for_each(output, &b->compositor->output_list, link)
			output->disable_planes ^= 1;
		weston_compositor_schedule_repaint(b->compositor);
		break;
	default:
		break;
	}
}

static struct qcom_backend *
qcom_backend_create(struct weston_compositor *compositor,
		    struct weston_qcom_backend_config *config)
{
	struct qcom_backend *b;

	weston_log("initializing qcom backend\n");

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;

	b->ion_fd = open("/dev/ion", O_RDONLY);
	if (b->ion_fd < 0) {
		weston_log("failed to open ion device: %m\n");
		goto err_free;
	}

	b->base.destroy = qcom_destroy;
	b->base.restore = qcom_restore;

	b->output_transform = config->output_transform;
	wl_list_init(&b->plane_list);

	if (pixman_renderer_init(compositor) < 0)
		goto err_ion;

	if (input_lh_init(&b->input, compositor) < 0) {
		weston_log("failed to create input devices\n");
		goto err_ion;
	}

	weston_compositor_set_presentation_clock(compositor, CLOCK_MONOTONIC);

	if (qcom_init_hw_info(b) < 0)
		goto err_input;

	if (qcom_output_create(b, config->device) < 0)
		goto err_input;

	if (linux_dmabuf_setup(compositor) < 0)
		weston_log("failed to initialize dmabuf support\n");

	compositor->backend = &b->base;

	weston_compositor_add_debug_binding(compositor, KEY_C,
					    debug_binding, b);

	return b;

err_ion:
	close(b->ion_fd);
err_input:
	input_lh_shutdown(&b->input);
err_free:
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_qcom_backend_config *config)
{
	config->device = "/dev/fb0";
	config->output_transform = WL_OUTPUT_TRANSFORM_NORMAL;
}

WL_EXPORT int
backend_init(struct weston_compositor *compositor,
	     struct weston_backend_config *config_base)
{
	struct qcom_backend *b;
	struct weston_qcom_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_QCOM_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_qcom_backend_config)) {
		weston_log("qcom backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = qcom_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	return 0;
}
