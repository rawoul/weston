#include <stdlib.h>

#include <wayland-server.h>
#include "fbx-text-server-protocol.h"

#include "lh-input.h"

struct fbx_text_binding {
	struct wl_resource *resource;
	struct fbx_text_context *context;
	struct wl_list link;
};

struct fbx_text_context {
	struct wl_global *global;
	struct wl_list binding_list;
	struct fbx_text_binding *target;
	struct input_lh *input;
};

void
fbx_text_set_focused_client(struct fbx_text_context *ft,
			struct wl_client *focused_client)
{
	struct fbx_text_binding *binding;
	struct weston_compositor *compositor;

	if (!ft)
		return;

	compositor = ft->input->compositor;

	wl_list_for_each(binding, &ft->binding_list, link) {
		struct wl_client *client =
			wl_resource_get_client(binding->resource);

		if (client == focused_client ||
		     client == compositor->input_method_client) {
			ft->target = binding;
			return;
		}
	}

	ft->target = NULL;
}

void fbx_text_inject(struct fbx_text_context *ft, int code)
{
	if (!ft->target)
		return;

	fbx_text_send_unicode(ft->target->resource,
			weston_compositor_get_time(), code);
}

static void
fbx_text_handle_destroy(struct wl_resource *resource)
{
	struct fbx_text_binding *binding =
		wl_resource_get_user_data(resource);

	if (binding->context->target == binding)
		binding->context->target = NULL;

	wl_list_remove(&binding->link);
	free(binding);
}

static void
bind_fbx_text(struct wl_client *client, void *data,
		 uint32_t version, uint32_t id)
{
	struct fbx_text_context *ft = data;
	struct fbx_text_binding *binding;

	binding = malloc(sizeof (*binding));
	if (!binding) {
		wl_client_post_no_memory(client);
		return;
	}

	binding->context = ft;
	binding->resource = wl_resource_create(client, &fbx_text_interface, 1, id);
	if (!binding->resource) {
		wl_client_post_no_memory(client);
		free(binding);
		return;
	}

	wl_list_insert(&ft->binding_list, &binding->link);

	wl_resource_set_implementation(binding->resource,
				       NULL, binding,
				       fbx_text_handle_destroy);
}

struct fbx_text_context *
fbx_text_init(struct input_lh *input)
{
	struct fbx_text_context *ft;

	ft = malloc(sizeof (*ft));
	if (!ft)
		return NULL;

	ft->input = input;
	ft->target = NULL;
	wl_list_init(&ft->binding_list);

	ft->global = wl_global_create(input->compositor->wl_display,
				      &fbx_text_interface,
				      1, ft, bind_fbx_text);

	return ft;
}

void fbx_text_destroy(struct fbx_text_context *ft)
{
	if (!ft)
		return;

	wl_global_destroy(ft->global);
	free(ft);
}
