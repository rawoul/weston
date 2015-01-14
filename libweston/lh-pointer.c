#include <stdlib.h>

#include <wayland-server.h>
#include "fbx-pointer-server-protocol.h"

#include "lh-input.h"
#include "shared/helpers.h"

struct fbx_pointer_binding {
	struct wl_resource *resource;
	struct fbx_pointer_context *context;
	int grab;
	struct wl_list link;
};

struct fbx_pointer_context {
	struct wl_global *global;
	struct wl_list binding_list;
	struct wl_listener show_input_panel_listener;
	struct wl_listener hide_input_panel_listener;
	struct wl_client *focused_client;
	bool input_panel_shown;
	bool available;
	struct input_lh *input;
};

void
fbx_pointer_set_available(struct fbx_pointer_context *fp, int available)
{
	struct fbx_pointer_binding *binding;

	if (!fp)
		return;

	if (fp->available == available)
		return;

	wl_list_for_each(binding, &fp->binding_list, link) {
		if (available)
			fbx_pointer_send_available(binding->resource);
		else
			fbx_pointer_send_unavailable(binding->resource);
	}

	fp->available = available;
}

static void
fbx_pointer_update_focus(struct fbx_pointer_context *fp)
{
	struct fbx_pointer_binding *binding;
	struct wl_client *focused_client;
	int grab = 0;

	if (fp->input_panel_shown)
		focused_client = fp->input->compositor->input_method_client;
	else
		focused_client = fp->focused_client;

	wl_list_for_each(binding, &fp->binding_list, link) {
		struct wl_client *client =
			wl_resource_get_client(binding->resource);

		if (binding->grab && client == focused_client) {
			grab = 1;
			break;
		}
	}

	input_lh_enable_pointer(fp->input, grab);
}

void
fbx_pointer_set_focused_client(struct fbx_pointer_context *fp,
			       struct wl_client *focused_client)
{
	if (fp) {
		fp->focused_client = focused_client;
		fbx_pointer_update_focus(fp);
	}
}

static void
fbx_pointer_grab(struct wl_client *client, struct wl_resource *resource)
{
	struct fbx_pointer_binding *binding =
		wl_resource_get_user_data(resource);
	struct input_lh *input = binding->context->input;

	if (!binding->grab) {
		binding->grab = 1;
		if (client == input->seat.focused_client ||
		    client == input->compositor->input_method_client)
			input_lh_enable_pointer(input, 1);
	}
}

static void
fbx_pointer_release(struct wl_client *client, struct wl_resource *resource)
{
	struct fbx_pointer_binding *binding =
		wl_resource_get_user_data(resource);
	struct input_lh *input = binding->context->input;

	if (binding->grab) {
		binding->grab = 0;
		if (client == input->seat.focused_client ||
		    client == input->compositor->input_method_client)
			input_lh_enable_pointer(input, 0);
	}
}

static const struct fbx_pointer_interface pointer_interface = {
	fbx_pointer_grab,
	fbx_pointer_release,
};

static void
fbx_pointer_handle_destroy(struct wl_resource *resource)
{
	struct fbx_pointer_binding *binding =
		wl_resource_get_user_data(resource);

	wl_list_remove(&binding->link);
	free(binding);
}

static void
bind_fbx_pointer(struct wl_client *client, void *data,
		 uint32_t version, uint32_t id)
{
	struct fbx_pointer_context *fp = data;
	struct fbx_pointer_binding *binding;

	binding = malloc(sizeof (*binding));
	if (!binding) {
		wl_client_post_no_memory(client);
		return;
	}

	binding->grab = 0;
	binding->context = fp;
	binding->resource = wl_resource_create(client,
					       &fbx_pointer_interface, 1, id);
	if (!binding->resource) {
		wl_client_post_no_memory(client);
		free(binding);
		return;
	}

	wl_list_insert(&fp->binding_list, &binding->link);

	wl_resource_set_implementation(binding->resource,
				       &pointer_interface, binding,
				       fbx_pointer_handle_destroy);

	if (fp->available)
		fbx_pointer_send_available(binding->resource);
	else
		fbx_pointer_send_unavailable(binding->resource);
}

static void
input_panel_hidden(struct wl_listener *listener, void *data)
{
	struct fbx_pointer_context *fp =
		container_of(listener, struct fbx_pointer_context,
			     hide_input_panel_listener);

	fp->input_panel_shown = false;
	fbx_pointer_update_focus(fp);
}

static void
input_panel_shown(struct wl_listener *listener, void *data)
{
	struct fbx_pointer_context *fp =
		container_of(listener, struct fbx_pointer_context,
			     show_input_panel_listener);

	fp->input_panel_shown = true;
	fbx_pointer_update_focus(fp);
}

struct fbx_pointer_context *
fbx_pointer_init(struct input_lh *input)
{
	struct fbx_pointer_context *fp;

	fp = malloc(sizeof (*fp));
	if (!fp)
		return NULL;

	fp->input = input;
	fp->available = false;
	fp->input_panel_shown = false;
	wl_list_init(&fp->binding_list);

	fp->show_input_panel_listener.notify = input_panel_shown;
	wl_signal_add(&input->compositor->show_input_panel_signal,
		      &fp->show_input_panel_listener);

	fp->hide_input_panel_listener.notify = input_panel_hidden;
	wl_signal_add(&input->compositor->hide_input_panel_signal,
		      &fp->hide_input_panel_listener);

	fp->global = wl_global_create(input->compositor->wl_display,
				      &fbx_pointer_interface,
				      1, fp, bind_fbx_pointer);

	return fp;
}

void fbx_pointer_destroy(struct fbx_pointer_context *fp)
{
	if (!fp)
		return;

	wl_list_remove(&fp->show_input_panel_listener.link);
	wl_list_remove(&fp->hide_input_panel_listener.link);
	wl_global_destroy(fp->global);
	free(fp);
}
