#ifndef _LH_INPUT_H_
# define _LH_INPUT_H_

#include <ela/ela.h>
#include <lh/context.h>

#include "config.h"
#include "compositor.h"

struct fbxbus_ctx;
struct hid_device;
struct wlh_device;
struct wlh_gamepad;
struct fbx_pointer_context;
struct fbx_text_context;
struct fbx_gamepad_context;

struct input_lh_device {
	struct input_lh *input;
	struct input_lh_seat *seat;
	struct lh_device *lh_device;
	struct hid_device *hid_device;
	struct wlh_device *wlh_device;
	struct wlh_gamepad *wlh_gamepad;
	struct wl_list link;
};

struct input_lh_seat {
	struct weston_seat base;
	struct input_lh *input;
	struct wl_listener caps_changed_listener;
	struct wl_listener keyboard_focus_listener;
	struct wl_client *focused_client;
	uint32_t konami_state;
};

struct input_lh {
	struct lh_ctx lh;
	struct lh_global_listener listener;
	struct ela_el *loop;
	struct fbxbus_ctx *bus;
	struct wl_event_source *fbxevent_source;
	struct weston_compositor *compositor;
	struct input_lh_seat seat;
	struct wl_list device_list;
	struct wl_event_source *regrab_idle;
	int pointer_enabled;
	int gamepad_enabled;
	struct fbx_pointer_context *fbx_pointer;
	struct fbx_gamepad_context *fbx_gamepad;
	struct fbx_text_context *fbx_text;
	struct wl_signal destroy_signal;
};

int input_lh_init(struct input_lh *input, struct weston_compositor *c);
struct input_lh_seat *input_lh_seat(struct weston_seat *seat);
void input_lh_enable_pointer(struct input_lh *input, int enable);
void input_lh_enable_gamepad(struct input_lh *input, int enable);
void input_lh_shutdown(struct input_lh *input);

struct hid_device *hid_device_new(struct input_lh *input,
				  struct input_lh_device *device);
void hid_device_set_grab(struct hid_device *hid_device,
			 struct wl_client *client, int grab);
void hid_device_destroy(struct hid_device *hid_device);

struct fbx_pointer_context *fbx_pointer_init(struct input_lh *input);
void fbx_pointer_destroy(struct fbx_pointer_context *fp);
void fbx_pointer_set_available(struct fbx_pointer_context *fp, int available);
void fbx_pointer_set_focused_client(struct fbx_pointer_context *fp,
				    struct wl_client *focused_client);

struct fbx_text_context *fbx_text_init(struct input_lh *input);
void fbx_text_destroy(struct fbx_text_context *ft);
void fbx_text_inject(struct fbx_text_context *ft, int code);
void fbx_text_set_focused_client(struct fbx_text_context *ft,
				struct wl_client *focused_client);

struct fbx_gamepad_context *fbx_gamepad_init(struct input_lh *input);
void fbx_gamepad_destroy(struct fbx_gamepad_context *fp);
void fbx_gamepad_set_focused_client(struct fbx_gamepad_context *fp,
				    struct wl_client *focused_client);

#endif
