#include <stdlib.h>
#include <stdint.h>

#include <lh/context.h>
#include <lh/device.h>
#include <lh/listener.h>
#include <lh/hid/report.h>

#include <wayland-server.h>

#include "hid-server-protocol.h"
#include "compositor.h"
#include "lh-input.h"

struct hid_device {
	struct input_lh *input;
	struct input_lh_device *device;
	struct lh_device *lh_device;
	struct wl_global *global;
	struct wl_list binding_list;
};

struct hid_binding {
	struct wl_resource *resource;
	struct hid_device *device;
	struct wl_list link;
	struct wl_list grab_list;
};

struct hid_grab {
	struct hid_binding *binding;
	struct lh_report_listener listener;
	struct wl_list link;
	int grab_count;
	uint8_t report_id;
};

static void
hid_grab_release(struct hid_grab *hid_grab)
{
	if (--hid_grab->grab_count > 0)
		return;

	lh_report_listener_release(&hid_grab->listener,
				   hid_grab->binding->device->lh_device);
}

static void
hid_grab_grab(struct hid_grab *hid_grab)
{
	if (hid_grab->grab_count++ > 0)
		return;

	lh_report_listener_grab(&hid_grab->listener,
				hid_grab->binding->device->lh_device,
				LHID_REPORT_INPUT, hid_grab->report_id);
}

static void
hid_grab_destroy(struct hid_grab *hid_grab)
{
	wl_list_remove(&hid_grab->link);
	hid_grab->grab_count = 0;
	hid_grab_release(hid_grab);

	lh_report_listener_deinit(&hid_grab->listener);
	free(hid_grab);
}

static void
hid_grab_report_input(struct lh_report_listener *listener,
		      const struct lhid_report *report)
{
	struct hid_grab *hid_grab =
		wl_container_of(listener, hid_grab, listener);

	struct wl_array data = {
		.data = (void *)report->data,
		.size = report->size,
		.alloc = report->size,
	};

	if (listener->way == LHID_REPORT_INPUT)
		wl_hid_device_send_input(hid_grab->binding->resource,
					 report->id, &data);
	else
		wl_hid_device_send_feature(hid_grab->binding->resource,
					   report->id, &data);
}

static void
hid_grab_report_lost(struct lh_report_listener *listener)
{
	struct hid_grab *hid_grab =
		wl_container_of(listener, hid_grab, listener);

	hid_grab_destroy(hid_grab);
}

static const struct lh_report_listener_handler hid_grab_report_listener = {
	hid_grab_report_input,
	hid_grab_report_lost,
};

static struct hid_grab *
hid_grab_create(struct hid_binding *hid_binding, uint8_t report_id)
{
	struct hid_grab *hid_grab;
	struct input_lh *input;
	struct weston_seat *seat;
	struct wl_client *client;

	hid_grab = zalloc(sizeof *hid_grab);
	if (!hid_grab)
		return NULL;

	hid_grab->binding = hid_binding;
	hid_grab->report_id = report_id;

	lh_report_listener_init(&hid_grab->listener, &hid_grab_report_listener);
	wl_list_insert(&hid_binding->grab_list, &hid_grab->link);

	client = wl_resource_get_client(hid_binding->resource);
	input = hid_binding->device->input;

	wl_list_for_each(seat, &input->compositor->seat_list, link) {
		struct input_lh_seat *lh_seat = input_lh_seat(seat);
		if (client == lh_seat->focused_client)
			hid_grab_grab(hid_grab);
	}

	return hid_grab;
}

static void
hid_device_grab(struct wl_client *client, struct wl_resource *resource,
		uint32_t report_id)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);
	struct hid_grab *hid_grab;

	if (!hid_binding)
		return;

	wl_list_for_each(hid_grab, &hid_binding->grab_list, link) {
		if (hid_grab->listener.report_id == report_id)
			return;
	}

	if (!hid_grab_create(hid_binding, report_id))
		wl_resource_post_no_memory(resource);
}

static void
hid_device_release(struct wl_client *client, struct wl_resource *resource,
		   uint32_t report_id)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);
	struct hid_grab *hid_grab;

	if (!hid_binding)
		return;

	wl_list_for_each(hid_grab, &hid_binding->grab_list, link) {
		if (hid_grab->report_id == report_id) {
			hid_grab_destroy(hid_grab);
			return;
		}
	}
}

static void
hid_device_feature(struct wl_client *client, struct wl_resource *resource,
		   int32_t report_id, struct wl_array *report_data)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);
	struct hid_device *hid_device = hid_binding->device;
	struct lh_ctx *lh = lh_device_context_get(hid_device->lh_device);
	struct lhid_report *report;
	lh_error_t err;

	if (!hid_binding)
		return;

	err = lh_report_alloc(lh, report_id, report_data->size,
			      report_data->data, &report);
	if (err) {
		wl_resource_post_no_memory(resource);
		return;
	}

	lh_device_send_feature_report(hid_device->lh_device, report);
	lh_report_refdrop(report);
}

static void
hid_device_output(struct wl_client *client, struct wl_resource *resource,
		  int32_t report_id, struct wl_array *report_data)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);
	struct hid_device *hid_device = hid_binding->device;
	struct lh_ctx *lh = lh_device_context_get(hid_device->lh_device);
	struct lhid_report *report;
	lh_error_t err;

	if (!hid_binding)
		return;

	err = lh_report_alloc(lh, report_id, report_data->size,
			      report_data->data, &report);
	if (err) {
		wl_resource_post_no_memory(resource);
		return;
	}

	lh_device_send_output_report(hid_device->lh_device, report);
	lh_report_refdrop(report);
}

static void
hid_device_feature_sollicit(struct wl_client *client,
			    struct wl_resource *resource, int32_t report_id)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);

	if (!hid_binding)
		return;

	lh_device_feature_sollicit(hid_binding->device->lh_device, report_id);
}

static void
hid_device_destroyed(struct wl_client *client, struct wl_resource *resource)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);

	if (hid_binding)
		hid_binding->resource = NULL;

	wl_resource_destroy(resource);
}

static const struct wl_hid_device_interface hid_device_interface = {
	hid_device_destroyed,
	hid_device_grab,
	hid_device_release,
	hid_device_feature,
	hid_device_output,
	hid_device_feature_sollicit,
};

static void
hid_binding_destroy(struct hid_binding *hid_binding)
{
	struct hid_grab *hid_grab, *next;

	if (hid_binding->resource) {
		wl_hid_device_send_dropped(hid_binding->resource);
		wl_resource_set_user_data(hid_binding->resource, NULL);
	}

	wl_list_for_each_safe(hid_grab, next, &hid_binding->grab_list, link)
		hid_grab_destroy(hid_grab);

	wl_list_remove(&hid_binding->link);
	free(hid_binding);
}

static void
destroy_hid_binding(struct wl_resource *resource)
{
	struct hid_binding *hid_binding = wl_resource_get_user_data(resource);

	if (hid_binding)
		hid_binding_destroy(hid_binding);
}

static void
bind_hid_device(struct wl_client *client, void *data,
		uint32_t version, uint32_t id)
{
	struct hid_device *hid_device = data;
	struct hid_binding *hid_binding;
	const struct lh_device_info *info;
	const struct lhid_descriptor *desc;

	hid_binding = zalloc(sizeof *hid_binding);
	if (!hid_binding) {
		wl_client_post_no_memory(client);
		return;
	}

	hid_binding->device = hid_device;
	hid_binding->resource =
		wl_resource_create(client, &wl_hid_device_interface, 1, id);
	if (!hid_binding->resource) {
		wl_client_post_no_memory(client);
		free(hid_binding);
		return;
	}

	wl_resource_set_implementation(hid_binding->resource,
				       &hid_device_interface,
				       hid_binding, destroy_hid_binding);

	wl_list_init(&hid_binding->grab_list);
	wl_list_insert(&hid_device->binding_list, &hid_binding->link);

	info = lh_device_info_get(hid_device->lh_device);
	desc = lh_device_descriptor_get(hid_device->lh_device);

	struct wl_array desc_array = {
		.data = (void *)desc->raw_desc,
		.size = desc->raw_desc_size,
	};

	struct wl_array raw_phys_array = {
		.data = (void *)desc->raw_phys,
		.size = desc->raw_phys_size,
	};

	struct wl_array string_array = {
		.data = (void *)desc->string,
		.size = desc->string_size,
	};

	wl_hid_device_send_description(hid_binding->resource,
				       info->name, info->serial,
				       info->bus, info->vid, info->pid,
				       info->version, &desc_array,
				       &raw_phys_array, &string_array);
}

void
hid_device_destroy(struct hid_device *hid_device)
{
	struct hid_binding *hid_binding, *next;

	wl_list_for_each_safe(hid_binding, next,
			      &hid_device->binding_list, link) {
		hid_binding_destroy(hid_binding);
	}

	wl_global_destroy(hid_device->global);
	free(hid_device);
}

void
hid_device_set_grab(struct hid_device *hid_device,
		    struct wl_client *client, int grab)
{
	struct hid_binding *hid_binding;
	struct hid_grab *hid_grab;

	wl_list_for_each(hid_binding, &hid_device->binding_list, link) {
		struct wl_client *binding_client =
			wl_resource_get_client(hid_binding->resource);

		if (binding_client != client)
			continue;

		wl_list_for_each(hid_grab, &hid_binding->grab_list, link) {
			if (grab)
				hid_grab_grab(hid_grab);
			else
				hid_grab_release(hid_grab);
		}
	}
}

struct hid_device *
hid_device_new(struct input_lh *input, struct input_lh_device *device)
{
	struct hid_device *hid_device;

	hid_device = zalloc(sizeof *hid_device);
	if (!hid_device)
		return NULL;

	hid_device->input = input;
	hid_device->device = device;
	hid_device->lh_device = device->lh_device;
	wl_list_init(&hid_device->binding_list);

	hid_device->global = wl_global_create(input->compositor->wl_display,
					      &wl_hid_device_interface,
					      1, hid_device, bind_hid_device);
	if (!hid_device->global) {
		free(hid_device);
		return NULL;
	}

	return hid_device;
}
