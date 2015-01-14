#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <linux/input.h>

#include <libfbxevent.h>
#include <libfbxbus.h>

#include <lh/context.h>
#include <lh/device.h>
#include <lh/listener.h>
#include <lh/enumerator/fbxdev.h>
#include <lh/enumerator/socket.h>
#include <lh/enumerator/rudp.h>
#include <lh/hid/descriptor.h>
#include <lh/hid/descriptor_walker.h>
#include <lh/hid/usage_page.h>
#include <lh/semantic/mapping.h>
#include <lh/semantic/usage_extractor.h>

#include <libfbxbus.h>
#include <fbxsystem.h>
#include <fbxmdnssd.h>

#include <wayland-server.h>

#include "ela-wayland.h"
#include "lh-input.h"
#include "shared/helpers.h"

#define UDP_HID_SRV	"_hid._udp"
#define UDP_HID_PORT	24322

#define SOCKET_NAME	"\0lh_devices.sock"

enum konami_code_state {
	KONAMI_IDLE,
	KONAMI_U,
	KONAMI_UU,
	KONAMI_UUD,
	KONAMI_UUDD,
	KONAMI_UUDDL,
	KONAMI_UUDDLR,
	KONAMI_UUDDLRL,
};

enum wlh_device_usage {
	WLH_USAGE_KEYBOARD = (1 << 0),
	WLH_USAGE_POINTER = (1 << 1),
};

enum wlh_event_type {
	WLH_EVENT_NONE,
	WLH_EVENT_ABS_MOTION,
	WLH_EVENT_REL_MOTION,
};

enum wlh_gamepad_control {
	WLH_GAMEPAD_X,
	WLH_GAMEPAD_Y,
	WLH_GAMEPAD_OK,
	WLH_GAMEPAD_BACK,
	WLH_GAMEPAD_MENU,
	WLH_GAMEPAD_INFO,
	WLH_GAMEPAD_POWER,
	WLH_GAMEPAD_HOME,
	WLH_GAMEPAD_PPLUS,
	WLH_GAMEPAD_PMINUS,
	WLH_GAMEPAD_VPLUS,
	WLH_GAMEPAD_VMINUS,
	WLH_GAMEPAD_MOUSE_X,
	WLH_GAMEPAD_MOUSE_Y,
	WLH_GAMEPAD_COUNT,
};

struct wlh_gamepad {
	struct input_lh *input;
	struct input_lh_seat *seat;
	struct lh_device *lh_device;
	struct lhs_mapping mapping;
	uint32_t gamepad_value[WLH_GAMEPAD_COUNT];
	int ok_pressed : 1;
	int click_pressed : 1;
	int32_t x, y;
	struct ela_event_source *motion_source;
};

struct wlh_pointer_item {
	struct wlh_pointer_report *report;
	struct lh_item_listener listener;
	struct wl_list link;
	const struct lhid_item *item;
};

struct wlh_pointer_report {
	struct wlh_device *device;
	struct lh_report_listener listener;
	struct wl_list item_list;
	struct wl_list link;
	uint8_t report_id;
};

struct wlh_device {
	struct input_lh *input;
	struct input_lh_seat *seat;
	struct lh_device *lh_device;

	uint32_t usage;

	enum wlh_event_type pending_event;

	struct wl_list pointer_report_list;
	int32_t abs_x, abs_y;
	int32_t rel_x, rel_y;
	int pointer_grabbed;

	struct lhs_usage_extractor usage_extractor;
};

/* Match wlh_gamepad_control entries */
static const struct lhs_need gamepad_mapping_desc[] = {
	{ "X",     LHID_UT(DESKTOP, X),      0, 2, LHID_PHYSICAL(THUMB, LEFT,  0)},
	{ "Y",     LHID_UT(DESKTOP, Y),      0, 2, LHID_PHYSICAL(THUMB, LEFT,  0)},

	{ "OK",    LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(THUMB, RIGHT, 0)},
	{ "Back",  LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(THUMB, RIGHT, 1)},
	{ "Menu",  LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(THUMB, RIGHT, 2)},
	{ "Info",  LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(THUMB, RIGHT, 3)},

	{ "Power", LHID_UT(DESKTOP, SELECT), 0, 1, LHID_PHYSICAL(THUMB, LEFT,  5)},
	{ "Home",  LHID_UT(DESKTOP, START),  0, 1, LHID_PHYSICAL(THUMB, RIGHT, 5)},

	{ "P+",    LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(INDEX_FINGER, RIGHT, 0)},
	{ "P-",    LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(INDEX_FINGER, RIGHT, 1)},
	{ "V+",    LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(INDEX_FINGER, LEFT, 0)},
	{ "V-",    LHID_UT(BUTTON, 0),       0, 1, LHID_PHYSICAL(INDEX_FINGER, LEFT, 1)},

	{ "mouse X", LHID_UT(DESKTOP, X), -5, 5, LHID_PHYSICAL(THUMB, LEFT,  4)},
	{ "mouse Y", LHID_UT(DESKTOP, Y), -5, 5, LHID_PHYSICAL(THUMB, LEFT,  4)},

	{ NULL },
};

static const unsigned char hid_keyboard[256] = {
	  0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
	 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
	 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
	 65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
	105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
	 72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
	191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
	115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
	122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,179,180,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,  0,  0,  0,  0,
	 29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
	150,158,159,128,136,177,178,176,142,152,173,140,  0,  0,  0,  0
};

static int init_fbxbus(struct input_lh *input);
static void shutdown_fbxbus(struct input_lh *input);

static void wlh_device_release_pointer(struct wlh_device *device);

static int
consumer2event(uint16_t consumer)
{
	switch (consumer) {
	case LHID_UT_CONSUMER_VOLUME_INCREMENT: return KEY_VOLUMEUP;
	case LHID_UT_CONSUMER_VOLUME_DECREMENT: return KEY_VOLUMEDOWN;
	case LHID_UT_CONSUMER_MUTE: return KEY_MUTE;
	case LHID_UT_CONSUMER_CHANNEL_INCREMENT: return KEY_CHANNELUP;
	case LHID_UT_CONSUMER_CHANNEL_DECREMENT: return KEY_CHANNELDOWN;
	case LHID_UT_CONSUMER_VCR_TV: return KEY_SCREEN;
	case LHID_UT_CONSUMER_RECORD: return KEY_RECORD;
	case LHID_UT_CONSUMER_FAST_FORWARD: return KEY_FASTFORWARD;
	case LHID_UT_CONSUMER_REWIND: return KEY_REWIND;
	case LHID_UT_CONSUMER_STOP: return KEY_STOP;
	case LHID_UT_CONSUMER_PLAY: return KEY_PLAY;
	case LHID_UT_CONSUMER_PLAY_PAUSE: return KEY_PLAYPAUSE;
	case LHID_UT_CONSUMER_PAUSE: return KEY_PAUSE;
	case LHID_UT_CONSUMER_SCAN_NEXT_TRACK: return KEY_NEXTSONG;
	case LHID_UT_CONSUMER_SCAN_PREVIOUS_TRACK: return KEY_PREVIOUSSONG;
	case LHID_UT_CONSUMER_FRAME_FORWARD: return KEY_FRAMEFORWARD;
	case LHID_UT_CONSUMER_FRAME_BACK: return KEY_FRAMEBACK;
	case LHID_UT_CONSUMER_DATA_ON_SCREEN: return KEY_SETUP;
	case LHID_UT_CONSUMER_SUB_CHANNEL_INCREMENT: return KEY_ANGLE;
	case LHID_UT_CONSUMER_ALTERNATE_AUDIO_INCREMENT: return KEY_LANGUAGE;
	case LHID_UT_CONSUMER_ALTERNATE_SUBTITLE_INCREMENT: return KEY_SUBTITLE;
	case LHID_UT_CONSUMER_EJECT: return KEY_EJECTCD;
	case LHID_UT_CONSUMER_POWER: return KEY_POWER;
	case LHID_UT_CONSUMER_MEDIA_SELECT_HOME: return KEY_HOMEPAGE;
	case LHID_UT_CONSUMER_RANDOM_PLAY: return KEY_SHUFFLE;
	case LHID_UT_CONSUMER_AC_ZOOM_IN: return KEY_ZOOMIN;
	case LHID_UT_CONSUMER_AC_ZOOM_OUT: return KEY_ZOOMOUT;

	case LHID_UT_CONSUMER_AC_BACK: return KEY_BACK;
	case LHID_UT_CONSUMER_AC_FORWARD: return KEY_FORWARD;
	case LHID_UT_CONSUMER_AC_REFRESH: return KEY_REFRESH;
	case LHID_UT_CONSUMER_AC_STOP: return KEY_STOP;

	case LHID_UT_CONSUMER_BLUE: return KEY_BLUE;
	case LHID_UT_CONSUMER_RED: return KEY_RED;
	case LHID_UT_CONSUMER_YELLOW: return KEY_YELLOW;
	case LHID_UT_CONSUMER_GREEN: return KEY_GREEN;

	case LHID_UT_CONSUMER_TOP_MENU: return BTN_TRIGGER_HAPPY12;
	case LHID_UT_CONSUMER_POPUP_MENU: return BTN_TRIGGER_HAPPY13;

	case LHID_UT_CONSUMER_MENU_PICK: return KEY_ENTER;
	case LHID_UT_CONSUMER_MENU_UP: return KEY_UP;
	case LHID_UT_CONSUMER_MENU_LEFT: return KEY_LEFT;
	case LHID_UT_CONSUMER_MENU_RIGHT: return KEY_RIGHT;
	case LHID_UT_CONSUMER_MENU_DOWN: return KEY_DOWN;
	case LHID_UT_CONSUMER_MENU_ESCAPE: return KEY_BACK;

	case LHID_UT_CONSUMER_AL_INTERNET_BROWSER: return KEY_WWW;
	case LHID_UT_CONSUMER_AL_AUDIO_BROWSER: return KEY_AUDIO;

	case LHID_UT_CONSUMER_AC_EXIT: return KEY_F2;
	case LHID_UT_CONSUMER_AC_SEARCH: return KEY_F1;
	case LHID_UT_CONSUMER_MENU: return KEY_F3;
	case LHID_UT_CONSUMER_AC_PROPERTIES: return KEY_F4;
	case LHID_UT_CONSUMER_AL_TASK_PROJECT_MANAGER: return KEY_HOMEPAGE;
	case LHID_UT_CONSUMER_AC_HOME: return KEY_HOMEPAGE;
	}
	return 0;
}

static void
konami_code_feed(struct input_lh_seat *input_seat,
		 uint32_t time, uint32_t keycode)
{
#define KONAMI_NEXT(key, state)					\
	if (keycode == (key)) {					\
		input_seat->konami_state = (state);		\
		return;						\
	}

#define KONAMI_LAST(key, nkey)					\
	if (keycode == (key)) {					\
		notify_key(&input_seat->base, time, (nkey),	\
			   WL_KEYBOARD_KEY_STATE_PRESSED,	\
			   STATE_UPDATE_NONE);			\
		notify_key(&input_seat->base, time, (nkey),	\
			   WL_KEYBOARD_KEY_STATE_RELEASED,	\
			   STATE_UPDATE_NONE);			\
	}

	switch (input_seat->konami_state) {
	case KONAMI_IDLE:
		KONAMI_NEXT(KEY_UP, KONAMI_U);
		break;
	case KONAMI_U:
		KONAMI_NEXT(KEY_UP, KONAMI_UU);
		break;
	case KONAMI_UU:
		KONAMI_NEXT(KEY_UP, KONAMI_UU);
		KONAMI_NEXT(KEY_DOWN, KONAMI_UUD);
		break;
	case KONAMI_UUD:
		KONAMI_NEXT(KEY_DOWN, KONAMI_UUDD);
		break;
	case KONAMI_UUDD:
		KONAMI_NEXT(KEY_LEFT, KONAMI_UUDDL);
		break;
	case KONAMI_UUDDL:
		KONAMI_NEXT(KEY_RIGHT, KONAMI_UUDDLR);
		break;
	case KONAMI_UUDDLR:
		KONAMI_NEXT(KEY_LEFT, KONAMI_UUDDLRL);
		break;
	case KONAMI_UUDDLRL:
		KONAMI_LAST(KEY_RIGHT, 0x21f);
		break;
	}

	input_seat->konami_state = KONAMI_IDLE;
}

static void
feed_key(struct input_lh_seat *input_seat, uint32_t usage, uint32_t value)
{
	struct weston_seat *seat = &input_seat->base;
	int code, state;

	code = 0;
	state = !!value;

	switch (usage >> 16) {
	case LHID_UT_KEYBOARD:
		code = hid_keyboard[usage & 0xff];
		break;

	case LHID_UT_CONSUMER:
		switch (usage & 0xffff) {
		case LHID_UT_CONSUMER_VOLUME:
			if (value != 0) {
				state = 2;
				code = value > 0 ?
					KEY_VOLUMEUP : KEY_VOLUMEDOWN;
			}
			break;

		default:
			code = consumer2event(usage);
			break;
		}
		break;

	case LHID_UT_DESKTOP:
		switch (usage & 0xffff) {
		case LHID_UT_DESKTOP_SYSTEM_SLEEP:
			code = KEY_SLEEP;
			break;
		case LHID_UT_DESKTOP_SYSTEM_WAKEUP:
			code = KEY_WAKEUP;
			break;
		case LHID_UT_DESKTOP_SYSTEM_APP_MENU:
		case LHID_UT_DESKTOP_SYSTEM_CONTEXT_MENU:
			code = KEY_F3;
			break;
		case LHID_UT_DESKTOP_POWER_DOWN:
			code = KEY_POWER;
			break;
		}
		break;

	case LHID_UT_DEVICE_CONTROLS:
		switch (usage & 0xffff) {
		case LHID_UT_DEVICE_CONTROLS_DISCOVER_WIRELESS_CONTROL:
			code = KEY_CONNECT;
			break;
		}
		break;
	}

	if (code != 0) {
		uint32_t time = weston_compositor_get_time();

		notify_key(seat, time, code, state ?
			   WL_KEYBOARD_KEY_STATE_PRESSED :
			   WL_KEYBOARD_KEY_STATE_RELEASED,
			   STATE_UPDATE_AUTOMATIC);

		if (state == 2) {
			state = 0;
			notify_key(seat, time, code,
				   WL_KEYBOARD_KEY_STATE_RELEASED,
				   STATE_UPDATE_AUTOMATIC);
		}

		if (state == 0)
			konami_code_feed(input_seat, time, code);
	}
}

static void
wlh_gamepad_destroy(struct wlh_gamepad *pad)
{
	lhs_mapping_deinit(&pad->mapping);
	ela_source_free(pad->input->loop, pad->motion_source);

	weston_seat_release_keyboard(&pad->seat->base);
	weston_seat_release_pointer(&pad->seat->base);

	free(pad);
}

static void mapping_value_changed(struct lhs_mapping *mapping,
				  const uint32_t *values)
{
	static const uint32_t x_map[] = {
		LHID_UT(CONSUMER, MENU_LEFT), 0, LHID_UT(CONSUMER, MENU_RIGHT),
	};
	static const uint32_t y_map[] = {
		LHID_UT(CONSUMER, MENU_UP), 0, LHID_UT(CONSUMER, MENU_DOWN),
	};
	static const uint32_t key_map[WLH_GAMEPAD_COUNT] = {
		[WLH_GAMEPAD_POWER] = LHID_UT(CONSUMER, POWER),
		[WLH_GAMEPAD_OK]    = LHID_UT(CONSUMER, MENU_PICK),
		[WLH_GAMEPAD_BACK]  = LHID_UT(CONSUMER, AC_BACK),
		[WLH_GAMEPAD_MENU]  = LHID_UT(CONSUMER, MENU),
		[WLH_GAMEPAD_INFO]  = LHID_UT(CONSUMER, AC_PROPERTIES),
		[WLH_GAMEPAD_HOME]  = LHID_UT(CONSUMER, AL_TASK_PROJECT_MANAGER),
		[WLH_GAMEPAD_PPLUS] = LHID_UT(CONSUMER, CHANNEL_INCREMENT),
		[WLH_GAMEPAD_PMINUS]= LHID_UT(CONSUMER, CHANNEL_DECREMENT),
		[WLH_GAMEPAD_VPLUS] = LHID_UT(CONSUMER, VOLUME_INCREMENT),
		[WLH_GAMEPAD_VMINUS]= LHID_UT(CONSUMER, VOLUME_DECREMENT),
	};

	struct wlh_gamepad *pad =
		container_of(mapping, struct wlh_gamepad, mapping);
	size_t control;
	int32_t x, y;

	for (control = 0; control < WLH_GAMEPAD_COUNT; control++) {
		uint32_t value = 0;
		uint32_t old_value = pad->gamepad_value[control];

		switch (control) {
		case WLH_GAMEPAD_X:
			value = x_map[values[control]];
			break;

		case WLH_GAMEPAD_Y:
			value = y_map[values[control]];
			break;

		case WLH_GAMEPAD_MOUSE_X:
		case WLH_GAMEPAD_MOUSE_Y:
			continue;

		default:
			if (values[control])
				value = key_map[control];
			break;
		}

		if (old_value == value)
			continue;

		pad->gamepad_value[control] = value;

		switch (control) {
		case WLH_GAMEPAD_MOUSE_X:
		case WLH_GAMEPAD_MOUSE_Y:
			break;

		case WLH_GAMEPAD_OK:
			notify_button(&pad->seat->base, 0,
				      BTN_LEFT, value ?
				      WL_POINTER_BUTTON_STATE_PRESSED :
				      WL_POINTER_BUTTON_STATE_RELEASED);
			break;

		default:
			/* Synthetize release */
			if (old_value)
				feed_key(pad->seat, old_value, 0);

			/* Synthetize press */
			if (value)
				feed_key(pad->seat, value, 1);
			break;
		}
	}

	x = (int32_t)values[WLH_GAMEPAD_MOUSE_X];
	y = (int32_t)values[WLH_GAMEPAD_MOUSE_Y];

	if ((pad->x || pad->y) && !(x || y))
		ela_remove(pad->input->loop, pad->motion_source);
	else if (!(pad->x || pad->y) && (x || y))
		ela_add(pad->input->loop, pad->motion_source);

	pad->x = x;
	pad->y = y;
}

static void
mapping_device_lost(struct lhs_mapping *mapping)
{
	struct wlh_gamepad *pad =
		container_of(mapping, struct wlh_gamepad, mapping);

	wlh_gamepad_destroy(pad);
}

static const struct lhs_mapping_handler gamepad_mapping_handler = {
	mapping_value_changed,
	mapping_device_lost,
};

static void
gamepad_handle_motion_timer(struct ela_event_source *source, int fd,
			    uint32_t mask, void *data)
{
	struct wlh_gamepad *pad = data;
	struct weston_pointer_motion_event event = { 0 };

	event = (struct weston_pointer_motion_event) {
		.mask = WESTON_POINTER_MOTION_REL,
		.dx = pad->x,
		.dy = pad->y,
	};

	notify_motion(&pad->seat->base, weston_compositor_get_time(), &event);
}

static struct wlh_gamepad *
register_gamepad(struct input_lh *input, struct input_lh_seat *seat,
		 struct lh_device *lh_device)
{
	struct wlh_gamepad *pad;
	struct timeval motion_interval = { 0, 1000 };

	pad = zalloc(sizeof *pad);
	if (!pad)
		return NULL;

	pad->input = input;
	pad->seat = seat;
	pad->lh_device = lh_device;

	if (lhs_mapping_init(&pad->mapping, &input->lh,
			     &gamepad_mapping_handler, pad->lh_device,
			     gamepad_mapping_desc)) {
		free(pad);
		return NULL;
	}

	ela_source_alloc(input->loop, gamepad_handle_motion_timer,
			 pad, &pad->motion_source);

	ela_set_timeout(input->loop, pad->motion_source, &motion_interval, 0);

	/* FIXME: only expose pointer when axis can be mapped */
	weston_seat_init_keyboard(&pad->seat->base, NULL);
	weston_seat_init_pointer(&pad->seat->base);

	return pad;
}

static void
wlh_device_remove_usage(struct wlh_device *device, uint32_t usage)
{
	usage &= device->usage;

	if (usage & WLH_USAGE_POINTER) {
		assert(wl_list_empty(&device->pointer_report_list));
		wlh_device_release_pointer(device);
		device->usage &= ~WLH_USAGE_POINTER;
	}

	if (usage & WLH_USAGE_KEYBOARD) {
		lhs_usage_extractor_deinit(&device->usage_extractor);
		weston_seat_release_keyboard(&device->seat->base);
		device->usage &= ~WLH_USAGE_KEYBOARD;
	}

	if (!device->usage)
		free(device);
}

static void
wlh_device_flush_pending_events(struct wlh_device *device, uint32_t time)
{
	struct input_lh_seat *seat;
	struct weston_pointer_motion_event event = { 0 };

	seat = device->seat;

	switch (device->pending_event) {
	case WLH_EVENT_NONE:
		return;

	case WLH_EVENT_REL_MOTION:
		event.mask = WESTON_POINTER_MOTION_REL;
		event.dx = device->rel_x;
		event.dy = device->rel_y;
		notify_motion(&seat->base, time, &event);
		device->rel_x = 0;
		device->rel_y = 0;
		break;

	case WLH_EVENT_ABS_MOTION:
		if (!seat->base.output)
			break;

		event.mask = WESTON_POINTER_MOTION_ABS;
		event.x = device->abs_x;
		event.y = device->abs_y;
		weston_output_transform_coordinate(seat->base.output,
						   event.x, event.y,
						   &event.x, &event.y);
		notify_motion(&seat->base, time, &event);
		break;
	}

	notify_pointer_frame(&seat->base);

	device->pending_event = WLH_EVENT_NONE;
}

static void
keyboard_handle_value(struct lhs_usage_extractor *ue,
		      const struct lhid_item *item,
		      uint32_t usage, uint32_t value)
{
	struct wlh_device *device =
		container_of(ue, struct wlh_device, usage_extractor);
	uint32_t time;

	time = weston_compositor_get_time();

	wlh_device_flush_pending_events(device, time);
	feed_key(device->seat, usage, value);
}

static int
keyboard_item_is_acceptable(struct lhs_usage_extractor *ue,
			    const struct lhid_item *item)
{
	if (lhid_item_is_constant(item->flags))
		return 0;

	switch (item->usage >> 16) {
	case LHID_UT_KEYBOARD:
	case LHID_UT_CONSUMER:
	case LHID_UT_DEVICE_CONTROLS:
		return 1;

	case LHID_UT_DESKTOP:
		switch (item->usage & 0xffff) {
		case LHID_UT_DESKTOP_SYSTEM_SLEEP:
		case LHID_UT_DESKTOP_SYSTEM_WAKEUP:
		case LHID_UT_DESKTOP_SYSTEM_CONTEXT_MENU:
		case LHID_UT_DESKTOP_SYSTEM_APP_MENU:
		case LHID_UT_DESKTOP_POWER_DOWN:
			return 1;
		}
		break;
	}

	return 0;
}

static void
keyboard_lost(struct lhs_usage_extractor *ue)
{
	struct wlh_device *device =
		container_of(ue, struct wlh_device, usage_extractor);

	wlh_device_remove_usage(device, WLH_USAGE_KEYBOARD);
}

static const struct lhs_usage_extractor_handler keyboard_ue_handler = {
	keyboard_handle_value,
	keyboard_item_is_acceptable,
	keyboard_lost,
};

static void
wlh_pointer_item_destroy(struct wlh_pointer_item *pi)
{
	struct wlh_device *device = pi->report->device;

	if (device->pointer_grabbed)
		lh_item_listener_release(&pi->listener, device->lh_device);

	lh_item_listener_deinit(&pi->listener);
	wl_list_remove(&pi->link);
	free(pi);
}

static void
wlh_pointer_report_destroy(struct wlh_pointer_report *pr)
{
	struct wlh_device *device = pr->device;

	assert(wl_list_empty(&pr->item_list));

	if (device->pointer_grabbed)
		lh_report_listener_release(&pr->listener, device->lh_device);

	lh_report_listener_deinit(&pr->listener);
	wl_list_remove(&pr->link);
	free(pr);
}

static void
wlh_pointer_report_grab(struct wlh_pointer_report *pr)
{
	struct wlh_pointer_item *pi;

	wl_list_for_each(pi, &pr->item_list, link)
		lh_item_listener_grab(&pi->listener, pr->device->lh_device,
				      pi->item);

	lh_report_listener_grab(&pr->listener, pr->device->lh_device,
				LHID_REPORT_INPUT, pr->report_id);
}

static void
wlh_pointer_report_release(struct wlh_pointer_report *pr)
{
	struct wlh_pointer_item *pi;

	lh_report_listener_release(&pr->listener, pr->device->lh_device);

	wl_list_for_each(pi, &pr->item_list, link)
		lh_item_listener_release(&pi->listener, pr->device->lh_device);
}

static void
process_absolute_motion(struct wlh_device *device,
			const struct lhid_item *item, int32_t value,
			uint32_t time)
{
	struct weston_output *output = device->seat->base.output;
	int screen_width, screen_height;

	if (!output)
		return;

	screen_width = output->current_mode->width;
	screen_height = output->current_mode->height;

	switch (item->usage) {
	case LHID_UT(DESKTOP, X):
		device->abs_x = (value - item->min) * screen_width /
		(item->max - item->min);
		break;
	case LHID_UT(DESKTOP, Y):
		device->abs_y = (value - item->min) * screen_height /
		(item->max - item->min);
		break;
	default:
		return;
	}

	if (device->pending_event == WLH_EVENT_NONE)
		device->pending_event = WLH_EVENT_ABS_MOTION;
}

static void
process_relative_motion(struct wlh_device *device,
			const struct lhid_item *item,
			int32_t value, uint32_t time)
{
	if (device->pending_event != WLH_EVENT_REL_MOTION)
		wlh_device_flush_pending_events(device, time);

	switch (item->usage) {
	case LHID_UT(DESKTOP, X):
		device->rel_x += value;
		break;
	case LHID_UT(DESKTOP, Y):
		device->rel_y += value;
		break;
	default:
		return;
	}

	device->pending_event = WLH_EVENT_REL_MOTION;
}

static void
process_axis(struct wlh_device *device,
	     const struct lhid_item *item,
	     int32_t vertical, int32_t horizontal, uint32_t time)
{
	struct input_lh_seat *seat;
	struct weston_pointer_axis_event event = { 0 };

	seat = device->seat;

	wlh_device_flush_pending_events(device, time);

	if (lhid_item_is_relative(item->flags)) {
		notify_axis_source(&seat->base, WL_POINTER_AXIS_SOURCE_WHEEL);
		event.axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
		event.value = -vertical * 5;
		notify_axis(&seat->base, time, &event);
	}
}

static void
pointer_item_input(struct lh_item_listener *listener,
		   uint32_t old, uint32_t value)
{
	struct wlh_pointer_item *pi =
		container_of(listener, struct wlh_pointer_item, listener);
	struct wlh_device *device;
	const struct lhid_item *item;
	struct input_lh_seat *seat;
	uint32_t time;

	device = pi->report->device;
	seat = device->seat;
	item = pi->item;

	time = weston_compositor_get_time();

	switch (item->usage) {
	case LHID_UT(DESKTOP, X):
	case LHID_UT(DESKTOP, Y):
		if (lhid_item_is_absolute(item->flags))
			process_absolute_motion(device, item, value, time);
		else
			process_relative_motion(device, item, value, time);
		break;

	case LHID_UT(DESKTOP, WHEEL):
		process_axis(device, item, value, 0, time);
		break;

	case LHID_UT(BUTTON, 0):
	case LHID_UT(BUTTON, 1):
	case LHID_UT(BUTTON, 2):
	case LHID_UT(BUTTON, 3):
	case LHID_UT(BUTTON, 4):
	case LHID_UT(BUTTON, 5):
	case LHID_UT(BUTTON, 6):
	case LHID_UT(BUTTON, 7):
		wlh_device_flush_pending_events(device, time);
		notify_button(&seat->base, time,
			      BTN_LEFT + (item->usage & 0x1f) - 1, value ?
			      WL_POINTER_BUTTON_STATE_PRESSED :
			      WL_POINTER_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
pointer_item_lost(struct lh_item_listener *listener)
{
	struct wlh_pointer_item *pi =
		container_of(listener, struct wlh_pointer_item, listener);

	wlh_pointer_item_destroy(pi);
}

static const struct lh_item_listener_handler pointer_item_listener_handler = {
	pointer_item_input,
	pointer_item_lost,
};

static void
pointer_report_input(struct lh_report_listener *listener,
		     const struct lhid_report *report)
{
	struct wlh_pointer_report *pr =
		container_of(listener, struct wlh_pointer_report, listener);
	uint32_t time;

	time = weston_compositor_get_time();

	wlh_device_flush_pending_events(pr->device, time);
}

static void
pointer_report_lost(struct lh_report_listener *listener)
{
	struct wlh_pointer_report *pr =
		container_of(listener, struct wlh_pointer_report, listener);
	struct wlh_device *device;

	device = pr->device;
	wlh_pointer_report_destroy(pr);

	if (wl_list_empty(&device->pointer_report_list))
		wlh_device_remove_usage(device, WLH_USAGE_POINTER);
}

static const struct lh_report_listener_handler pointer_report_listener_handler = {
	pointer_report_input,
	pointer_report_lost,
};

static int
wlh_device_add_pointer_report(struct wlh_device *device,
			      const struct lhid_report_desc *report)
{
	struct wlh_pointer_report *pr;
	struct wlh_pointer_item *pi;
	size_t i;

	pr = zalloc(sizeof *pr);
	if (!pr)
		return -1;

	pr->device = device;
	wl_list_init(&pr->item_list);

	for (i = 0; i < report->item_count; i++) {
		const struct lhid_item *item = &report->item[i];

		switch (item->usage) {
		case LHID_UT(BUTTON, 0):
		case LHID_UT(BUTTON, 1):
		case LHID_UT(BUTTON, 2):
		case LHID_UT(BUTTON, 3):
		case LHID_UT(BUTTON, 4):
		case LHID_UT(BUTTON, 5):
		case LHID_UT(BUTTON, 6):
		case LHID_UT(BUTTON, 7):
		case LHID_UT(DESKTOP, X):
		case LHID_UT(DESKTOP, Y):
		case LHID_UT(DESKTOP, WHEEL):
			break;
		default:
			continue;
		}

		pi = zalloc(sizeof *pi);
		if (!pi)
			continue;

		pi->item = item;
		pi->report = pr;

		lh_item_listener_init(&pi->listener,
				      &pointer_item_listener_handler);

		wl_list_insert(&pr->item_list, &pi->link);
	}

	if (wl_list_empty(&pr->item_list)) {
		free(pr);
		return -1;
	}

	pr->report_id = report->item[0].decoder.report_id;

	lh_report_listener_init(&pr->listener,
				&pointer_report_listener_handler);

	wl_list_insert(&device->pointer_report_list, &pr->link);

	return 0;
}

static void
wlh_device_grab_pointer(struct wlh_device *device)
{
	struct wlh_pointer_report *pr;

	if (device->pointer_grabbed || !(device->usage & WLH_USAGE_POINTER))
		return;

	device->pointer_grabbed = 1;

	wl_list_for_each(pr, &device->pointer_report_list, link)
		wlh_pointer_report_grab(pr);

	weston_seat_init_pointer(&device->seat->base);
}

static void
wlh_device_release_pointer(struct wlh_device *device)
{
	struct wlh_pointer_report *pr;

	if (!device->pointer_grabbed)
		return;

	device->pointer_grabbed = 0;

	wl_list_for_each(pr, &device->pointer_report_list, link)
		wlh_pointer_report_release(pr);

	weston_seat_release_pointer(&device->seat->base);
}

static struct wlh_device *
register_device(struct input_lh *input, struct input_lh_seat *seat,
		struct lh_device *lh_device)
{
	const struct lh_device_info *info;
	const struct lhid_descriptor *desc;
	struct wlh_device *device;
	size_t i;

	device = zalloc(sizeof *device);
	if (!device)
		return NULL;

	device->input = input;
	device->seat = seat;
	device->lh_device = lh_device;
	device->pending_event = WLH_EVENT_NONE;

	/* register keyboard related reports */
	if (!lhs_usage_extractor_init(&device->usage_extractor,
				      &keyboard_ue_handler,
				      device->lh_device))
		device->usage |= WLH_USAGE_KEYBOARD;

	/* register pointer related reports */
	wl_list_init(&device->pointer_report_list);

	desc = lh_device_descriptor_get(device->lh_device);
	for (i = 0; i < desc->way[LHID_REPORT_INPUT].desc_count; i++) {
		const struct lhid_report_desc *rd =
			&desc->way[LHID_REPORT_INPUT].desc[i];

		wlh_device_add_pointer_report(device, rd);
	}

	if (!wl_list_empty(&device->pointer_report_list))
		device->usage |= WLH_USAGE_POINTER;

	/* throw out device if it cannot be used for anything useful */
	if (!device->usage) {
		free(device);
		return NULL;
	}

	/* register device in weston */
	if (device->usage & WLH_USAGE_KEYBOARD)
		weston_seat_init_keyboard(&device->seat->base, NULL);

	/* only grab remote controller pointer when actually
	 * needed, to avoid using the battery too much */
	info = lh_device_info_get(lh_device);
	if (info->bus != LH_BUS_RTI || input->pointer_enabled)
		wlh_device_grab_pointer(device);

	return device;
}

static int
device_is_gamepad(struct lh_device *lh_device)
{
	const struct lhid_descriptor *desc;
	size_t i;

	desc = lh_device_descriptor_get(lh_device);
	for (i = 0; i < desc->way[LHID_REPORT_INPUT].desc_count; i++) {
		const struct lhid_report_desc *rd =
			&desc->way[LHID_REPORT_INPUT].desc[i];

		switch (rd->usage) {
		case LHID_UT(DESKTOP, GAME_PAD):
		case LHID_UT(DESKTOP, JOYSTICK):
			return 1;
		}
	}

	return 0;
}

static struct input_lh_device *
input_lh_device_new(struct input_lh *input, struct lh_device *lh_device)
{
	struct input_lh_device *device;
	const struct lh_device_info *info;

	device = zalloc(sizeof *device);
	if (!device)
		return NULL;

	device->input = input;
	device->seat = &input->seat;
	device->lh_device = lh_device;
	device->hid_device = hid_device_new(input, device);

	wl_list_insert(&input->device_list, &device->link);

	info = lh_device_info_get(lh_device);

	if (device_is_gamepad(lh_device) &&
	    (device->wlh_gamepad =
		     register_gamepad(input, &input->seat, lh_device))) {
		weston_log("using input device %s as a gamepad\n", info->name);

	} else if ((device->wlh_device =
		    register_device(input, &input->seat, lh_device))) {
		weston_log("using input device %s\n", info->name);
	} else {
		weston_log("not using input device %s\n", info->name);
	}

	return device;
}

static void
input_lh_device_destroy(struct input_lh_device *device)
{
	wl_list_remove(&device->link);

	if (device->hid_device)
		hid_device_destroy(device->hid_device);

	if (device->lh_device)
		lh_device_close(device->lh_device);

	free(device);
}

static void
device_new(struct lh_global_listener *listener, struct lh_device *lh_device)
{
	struct input_lh *input =
		container_of(listener, struct input_lh, listener);

	input_lh_device_new(input, lh_device);
}

static void
device_dropped(struct lh_global_listener *listener, struct lh_device *lh_device)
{
	struct input_lh *input = wl_container_of(listener, input, listener);
	struct input_lh_device *device;

	wl_list_for_each(device, &input->device_list, link) {
		if (device->lh_device == lh_device) {
			device->lh_device = NULL;
			input_lh_device_destroy(device);
			return;
		}
	}
}

static const struct lh_global_listener_handler global_handler = {
	device_new,
	device_dropped,
};

static int
publish_hid_service(struct input_lh *input)
{
	if (input->mdns_published) {
		fbxmdnssd_remove(input->bus, input->mdns_id);
		input->mdns_published = 0;
	}

	if (!input->mdns_name)
		return 0;

	weston_log("registering hid mDNS service with name %s\n",
		   input->mdns_name);

	if (fbxmdnssd_publish(input->bus, input->mdns_name,
			      UDP_HID_SRV, UDP_HID_PORT,
			      FBXMDNSSD_PROTOCOL_ALL,
			      &input->mdns_id) != FBXMDNSSD_SUCCESS)
		return -1;

	input->mdns_published = 1;

	return 0;
}

static int
set_network_name(struct input_lh *input, const char *name)
{
	free(input->mdns_name);
	input->mdns_name = name ? strdup(name) : NULL;

	return publish_hid_service(input);
}

static void
handle_system_name(void *data, const char *name,
		   const char *dns_name,
		   const char *mdns_name,
		   const char *netbios_name)
{
	struct input_lh *input = data;

	set_network_name(input, name);
}

static void
handle_mdnssd_startup(struct fbxbus_msg *msg, void *data)
{
	struct input_lh *input = data;
	const char *srvname;

	srvname = fbxbus_msg_get_str(msg);
	if (!srvname || strcmp(srvname, "fbxmdnssd"))
		return;

	publish_hid_service(input);
}

static void
handle_mdnssd_exit(struct fbxbus_msg *msg, void *data)
{
	struct input_lh *input = data;
	const char *srvname;

	srvname = fbxbus_msg_get_str(msg);
	if (!srvname || strcmp(srvname, "fbxmdnssd"))
		return;

	input->mdns_published = 0;
}

static int
enumerate_kernel_devices(struct input_lh *input)
{
	struct lh_enumerator *e;

	lh_enumerator_fbxdev_init(&input->lh, input->bus, input->loop, &e);

	return 0;
}

static int
enumerate_user_devices(struct input_lh *input)
{
	struct lh_enumerator *e;
	struct sockaddr_un sunaddr;
	socklen_t socklen;
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) < 0) {
		weston_log("failed to create lh socket: %m\n");
		return -1;
	}

	sunaddr.sun_family = AF_UNIX;
	socklen = sizeof SOCKET_NAME;
	memcpy(sunaddr.sun_path, SOCKET_NAME, socklen);
	socklen += offsetof(struct sockaddr_un, sun_path);

	if (bind(fd, (struct sockaddr *)&sunaddr, socklen) < 0) {
		weston_log("failed to bind lh socket: %m\n");
		close(fd);
		return -1;
	}

	lh_enumerator_socket_create(&input->lh, input->loop, fd, &e);

	return 0;
}

static int
enumerate_network_devices(struct input_lh *input)
{
	struct lh_enumerator *e;
	char *name;

	if (lh_rudp_server_create(&input->lh, input->loop, UDP_HID_PORT, &e)) {
		weston_log("failed to create rudp server\n");
		return -1;
	}

	fbxsystem_register_name_changed(input->bus, handle_system_name, input);
	if (!fbxsystem_name_get(input->bus, &name)) {
		set_network_name(input, name);
		free(name);
	}

	fbxbus_register(input->bus, FBXBUS_SIGNAL, FBXBUS_DAEMON_MSG_PATH,
			"name_acquired", handle_mdnssd_startup, input);

	fbxbus_register(input->bus, FBXBUS_SIGNAL, FBXBUS_DAEMON_MSG_PATH,
			"name_lost", handle_mdnssd_exit, input);

	return 0;
}

static void
seat_caps_changed(struct wl_listener *listener, void *data)
{
	struct input_lh_seat *seat =
		container_of(listener, struct input_lh_seat,
			     caps_changed_listener);
	struct weston_keyboard *keyboard =
		weston_seat_get_keyboard(&seat->base);

	if (keyboard) {
		if (wl_list_empty(&seat->keyboard_focus_listener.link))
			wl_signal_add(&keyboard->focus_signal,
				      &seat->keyboard_focus_listener);
	} else {
		wl_list_init(&seat->keyboard_focus_listener.link);
	}
}

static void
idle_regrab(void *data)
{
	struct input_lh *input = data;
	struct input_lh_seat *seat = &input->seat;
	struct weston_keyboard *keyboard =
		weston_seat_get_keyboard(&seat->base);
	struct wl_client *focused_client;

	if (input->regrab_idle) {
		wl_event_source_remove(input->regrab_idle);
		input->regrab_idle = NULL;
	}

	if (!keyboard || !keyboard->focus || !keyboard->focus->resource)
		focused_client = NULL;
	else
		focused_client =
			wl_resource_get_client(keyboard->focus->resource);

	if (seat->focused_client != focused_client) {
		struct input_lh_device *device;

		wl_list_for_each(device, &input->device_list, link) {
			if (!device->hid_device)
				continue;

			hid_device_set_grab(device->hid_device,
					    seat->focused_client, 0);
			hid_device_set_grab(device->hid_device,
					    focused_client, 1);
		}

		seat->focused_client = focused_client;
	}
}

static void
handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct input_lh_seat *seat = input_lh_seat(keyboard->seat);
	struct input_lh *input = seat->input;
	struct wl_event_loop *loop;

	if (input->regrab_idle)
		return;

	/*
	 * Defer regrab, to the next loop iteration. Without this we
	 * might add or remove devices while in a signal handler, which
	 * will confuse the next listeners.
	 */
	loop = wl_display_get_event_loop(input->compositor->wl_display);
	input->regrab_idle = wl_event_loop_add_idle(loop, idle_regrab, input);
}

void
input_lh_enable_pointer(struct input_lh *input, int enable)
{
	struct input_lh_device *device;
	const struct lh_device_info *info;

	wl_list_for_each(device, &input->device_list, link) {
		if (!device->wlh_device)
			continue;

		info = lh_device_info_get(device->wlh_device->lh_device);
		if (info->bus != LH_BUS_RTI)
			continue;

		if (enable)
			wlh_device_grab_pointer(device->wlh_device);
		else
			wlh_device_release_pointer(device->wlh_device);
	}

	input->pointer_enabled = enable;
}

struct input_lh_seat *
input_lh_seat(struct weston_seat *seat)
{
	struct wl_listener *listener;

	listener = wl_signal_get(&seat->updated_caps_signal,
				 seat_caps_changed);
	assert(listener != NULL);

	return container_of(listener,
			    struct input_lh_seat, caps_changed_listener);
}

static void
input_lh_log(struct lh_ctx *lh, enum lh_log_level level,
	     const char *fmt, va_list ap)
{
	if (level <= LH_LOG_DEBUG)
		return;

	weston_log("lh: ");
	weston_vlog_continue(fmt, ap);
}

static const struct lh_handler lh_handler = {
	input_lh_log,
	lh_mem_alloc_default,
	lh_mem_free_default,
};

int
input_lh_init(struct input_lh *input, struct weston_compositor *c)
{
	struct wl_event_loop *loop;

	memset(input, 0, sizeof (*input));

	if (lh_init(&input->lh, &lh_handler)) {
		weston_log("failed to init lh context\n");
		return -1;
	}

	input->compositor = c;

	if (init_fbxbus(input))
		goto err_init;

	loop = wl_display_get_event_loop(c->wl_display);
	input->loop = ela_wayland_create(loop);

	wl_list_init(&input->device_list);

	weston_seat_init(&input->seat.base, c, "default");
	input->seat.input = input;

	input->seat.keyboard_focus_listener.notify = handle_keyboard_focus;
	wl_list_init(&input->seat.keyboard_focus_listener.link);

	input->seat.caps_changed_listener.notify = seat_caps_changed;
	wl_signal_add(&input->seat.base.updated_caps_signal,
		      &input->seat.caps_changed_listener);

	lh_global_listener_add(&input->listener, &global_handler, &input->lh);

	enumerate_kernel_devices(input);
	enumerate_user_devices(input);
	enumerate_network_devices(input);

	return 0;

err_init:
	lh_deinit(&input->lh);
	return -1;
}

void
input_lh_shutdown(struct input_lh *input)
{
	fbxsystem_register_name_changed(input->bus, NULL, NULL);

	fbxbus_unregister(input->bus, FBXBUS_SIGNAL, FBXBUS_DAEMON_MSG_PATH,
			  "name_acquired");

	fbxbus_unregister(input->bus, FBXBUS_SIGNAL, FBXBUS_DAEMON_MSG_PATH,
			  "name_lost");

	set_network_name(input, NULL);

	lh_deinit(&input->lh);
	ela_close(input->loop);
	shutdown_fbxbus(input);
	weston_seat_release(&input->seat.base);

	if (input->regrab_idle) {
		wl_event_source_remove(input->regrab_idle);
		input->regrab_idle = NULL;
	}
}

static int
dispatch_fbxevent(int fd, uint32_t mask, void *data)
{
	struct fbxevent_ctx *evctx = data;

	fbxevent_wait(evctx);

	return 1;
}

static int
init_fbxbus(struct input_lh *input)
{
	struct fbxevent_ctx *evctx;
	struct wl_event_loop *loop;

	if ((evctx = fbxevent_init()) == NULL) {
		weston_log("failed fbxevent init");
		return -1;
	}

	loop = wl_display_get_event_loop(input->compositor->wl_display);

	input->fbxevent_source =
		wl_event_loop_add_fd(loop, fbxevent_get_fd(evctx),
				     WL_EVENT_READABLE | WL_EVENT_WRITABLE,
				     dispatch_fbxevent, evctx);

	if (input->fbxevent_source == NULL) {
		weston_log("failed to create source for fbxevent");
		goto fail;
	}

	if ((input->bus = fbxbus_create(evctx)) == NULL) {
		weston_log("failed fbxbus init");
		goto fail;
	}

	if (fbxbus_connect(input->bus) < 0) {
		weston_log("failed to connect to fbxbus: %s",
			   fbxbus_get_strerror(input->bus));
		goto fail;
	}

	return 0;

fail:
	if (!input->bus)
		fbxevent_destroy(evctx);
	shutdown_fbxbus(input);

	return -1;
}

static void
shutdown_fbxbus(struct input_lh *input)
{
	if (input->fbxevent_source) {
		wl_event_source_remove(input->fbxevent_source);
		input->fbxevent_source = NULL;
	}

	if (input->bus) {
		struct fbxevent_ctx *evctx = fbxbus_get_event_ctx(input->bus);

		fbxbus_release(input->bus);
		fbxevent_destroy(evctx);
		input->bus = NULL;
	}
}
