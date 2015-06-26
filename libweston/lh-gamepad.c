#include <stdlib.h>

#include <wayland-server.h>
#include "fbx-gamepad-server-protocol.h"

#include "lh-input.h"

struct fbx_gamepad_binding {
	struct wl_resource *resource;
	struct fbx_gamepad_context *context;
	int grab;
	struct wl_list link;
};

struct fbx_gamepad_context {
	struct wl_global *global;
	struct wl_list binding_list;
	struct input_lh *input;
};

void
fbx_gamepad_set_focused_client(struct fbx_gamepad_context *fg,
			       struct wl_client *focused_client)
{
	struct fbx_gamepad_binding *binding;
	int grab = 0;

	if (!fg)
		return;

	wl_list_for_each(binding, &fg->binding_list, link) {
		struct wl_client *client =
			wl_resource_get_client(binding->resource);

		if (binding->grab && client == focused_client) {
			grab = 1;
			break;
		}
	}

	input_lh_enable_gamepad(fg->input, grab);
}

static void
fbx_gamepad_grab(struct wl_client *client, struct wl_resource *resource)
{
	struct fbx_gamepad_binding *binding =
		wl_resource_get_user_data(resource);
	struct input_lh *input = binding->context->input;

	if (!binding->grab) {
		binding->grab = 1;
		if (client == input->seat.focused_client)
			input_lh_enable_gamepad(input, 1);
	}
}

static void
fbx_gamepad_release(struct wl_client *client, struct wl_resource *resource)
{
	struct fbx_gamepad_binding *binding =
		wl_resource_get_user_data(resource);
	struct input_lh *input = binding->context->input;

	if (binding->grab) {
		binding->grab = 0;
		if (client == input->seat.focused_client)
			input_lh_enable_gamepad(input, 0);
	}
}

static const struct fbx_gamepad_interface gamepad_interface = {
	fbx_gamepad_grab,
	fbx_gamepad_release,
};

static void
fbx_gamepad_handle_destroy(struct wl_resource *resource)
{
	struct fbx_gamepad_binding *binding =
		wl_resource_get_user_data(resource);

	wl_list_remove(&binding->link);
	free(binding);
}

static void
bind_fbx_gamepad(struct wl_client *client, void *data,
		 uint32_t version, uint32_t id)
{
	struct fbx_gamepad_context *fg = data;
	struct fbx_gamepad_binding *binding;

	binding = malloc(sizeof (*binding));
	if (!binding) {
		wl_client_post_no_memory(client);
		return;
	}

	binding->grab = 0;
	binding->context = fg;
	binding->resource = wl_resource_create(client,
					       &fbx_gamepad_interface, 1, id);
	if (!binding->resource) {
		wl_client_post_no_memory(client);
		free(binding);
		return;
	}

	wl_list_insert(&fg->binding_list, &binding->link);

	wl_resource_set_implementation(binding->resource,
				       &gamepad_interface, binding,
				       fbx_gamepad_handle_destroy);
}

struct fbx_gamepad_context *
fbx_gamepad_init(struct input_lh *input)
{
	struct fbx_gamepad_context *fg;

	fg = malloc(sizeof (*fg));
	if (!fg)
		return NULL;

	fg->input = input;
	wl_list_init(&fg->binding_list);

	fg->global = wl_global_create(input->compositor->wl_display,
				      &fbx_gamepad_interface,
				      1, fg, bind_fbx_gamepad);

	return fg;
}

void fbx_gamepad_destroy(struct fbx_gamepad_context *fg)
{
	if (!fg)
		return;

	wl_global_destroy(fg->global);
	free(fg);
}
