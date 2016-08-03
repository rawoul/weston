#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/fb.h>
#include <linux/msm_mdp.h>

#include "compositor-qcom.h"
#include "compositor.h"
#include "lh-input.h"
#include "pixman-renderer.h"
#include "presentation-time-server-protocol.h"
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

	struct qcom_hwinfo hwinfo;
	struct wl_list plane_list;
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
	struct wl_event_source *finish_frame_timer;

	/* Frame buffer details. */
	char *device;
	int fd;
	struct qcom_screeninfo fb_info;
	void *fb; /* length is fb_info.buffer_length */

	/* pixman details. */
	pixman_image_t *hw_surface;
	uint8_t depth;
};

struct qcom_plane {
	struct weston_plane base;
	struct wl_list link;
	uint32_t index;
	enum mdp_overlay_pipe_type type;
};

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

static int
qcom_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct qcom_output *output = qcom_output(base);
	struct qcom_backend *backend = output->backend;
	struct weston_compositor *ec = backend->compositor;

	/* Repaint the damaged region onto the back buffer. */
	pixman_renderer_output_set_buffer(base, output->hw_surface);
	ec->renderer->repaint_output(base, damage);

	/* Update the damage region. */
	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	/* Schedule the end of the frame. We do not sync this to the frame
	 * buffer clock because users who want that should be using the DRM
	 * compositor. FBIO_WAITFORVSYNC blocks and FB_ACTIVATE_VBL requires
	 * panning, which is broken in most kernel drivers.
	 *
	 * Finish the frame synchronised to the specified refresh rate. The
	 * refresh rate is given in mHz and the interval in ms. */
	wl_event_source_timer_update(output->finish_frame_timer,
				     1000000 / output->mode.refresh);

	return 0;
}

static int
finish_frame_handler(void *data)
{
	struct qcom_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor,
						  &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
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

	if (munmap(output->fb, output->fb_info.buffer_length) < 0)
		weston_log("failed to munmap frame buffer: %m\n");

	output->fb = NULL;

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
qcom_fbdev_map(struct qcom_output *output)
{
	weston_log("mapping fbdev frame buffer\n");

	output->fb = mmap(NULL, output->fb_info.buffer_length,
	                  PROT_WRITE, MAP_SHARED, output->fd, 0);
	if (output->fb == MAP_FAILED) {
		weston_log("failed to mmap frame buffer: %m\n");
		return -1;
	}

	output->hw_surface =
		pixman_image_create_bits(output->fb_info.pixel_format,
		                         output->fb_info.x_resolution,
		                         output->fb_info.y_resolution,
		                         output->fb,
		                         output->fb_info.line_length);
	if (output->hw_surface == NULL) {
		weston_log("failed to create surface for frame buffer\n");
		goto fail;
	}

	return 0;

fail:
	qcom_fbdev_destroy(output);
	return -1;
}

/* NOTE: This leaves output->fb_info populated, caching data so that if
 * qcom_output_reenable() is called again, it can determine whether a mode-set
 * is needed. */
static void
qcom_output_disable(struct weston_output *base)
{
	struct qcom_output *output = qcom_output(base);

	weston_log("disabling fbdev output\n");

	if (output->hw_surface != NULL) {
		pixman_image_unref(output->hw_surface);
		output->hw_surface = NULL;
	}

	qcom_fbdev_destroy(output);
}

static void
qcom_output_destroy(struct weston_output *base)
{
	struct qcom_output *output = qcom_output(base);

	weston_log("destroying fbdev output\n");

	qcom_output_disable(base);

	if (base->renderer_state != NULL)
		pixman_renderer_output_destroy(base);

	weston_output_destroy(&output->base);

	free(output->device);
	free(output);
}

static int
qcom_output_create(struct qcom_backend *backend, const char *device)
{
	struct qcom_output *output;
	struct wl_event_loop *loop;

	weston_log("creating fbdev output\n");

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->backend = backend;
	output->device = strdup(device);

	if (qcom_fbdev_open(output, device, &output->fb_info) < 0)
		goto out_free_early;

	if (qcom_fbdev_map(output) < 0)
		goto out_free_early;

	output->base.start_repaint_loop = qcom_output_start_repaint_loop;
	output->base.repaint = qcom_output_repaint;
	output->base.destroy = qcom_output_destroy;

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

	weston_output_init(&output->base, backend->compositor,
	                   0, 0, 0, 0, backend->output_transform, 1);

	if (pixman_renderer_output_create(&output->base) < 0)
		goto out_free_output;

	loop = wl_display_get_event_loop(backend->compositor->wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	weston_compositor_add_output(backend->compositor, &output->base);

	weston_log("fbdev output %d×%d@%dHz\n",
		   output->mode.width, output->mode.height,
		   output->mode.refresh / 1000);

	return 0;

out_free_output:
	qcom_output_destroy(&output->base);
	return -1;

out_free_early:
	qcom_fbdev_destroy(output);
	free(output->device);
	free(output);
	return -1;
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

	input_lh_shutdown(&b->input);
	weston_compositor_shutdown(compositor);
	free(b);
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

	b->base.destroy = qcom_destroy;
	b->base.restore = qcom_restore;

	b->output_transform = config->output_transform;
	wl_list_init(&b->plane_list);

	if (pixman_renderer_init(compositor) < 0)
		goto err_free;

	if (input_lh_init(&b->input, compositor) < 0) {
		weston_log("failed to create input devices\n");
		goto err_free;
	}

	if (qcom_init_hw_info(b) < 0)
		goto err_input;

	if (qcom_output_create(b, config->device) < 0)
		goto err_input;

	compositor->backend = &b->base;
	return b;

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
