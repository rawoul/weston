#include "config.h"
#include "lh-udp.h"

#ifdef ENABLE_LH_UDP

#include <string.h>
#include <stdlib.h>

#include <lh/enumerator/rudp.h>

#include <libfbxbus.h>
#include <fbxsystem.h>
#include <fbxmdnssd.h>

#include "lh-input.h"
#include "shared/helpers.h"

#define UDP_HID_SRV	"_hid._udp"
#define UDP_HID_PORT	24322

struct input_lh_udp {
	struct input_lh *input;
	char *mdns_name;
	bool mdns_published;
	unsigned mdns_id;
	struct fbxbus_rule *name_acquired_listener;
	struct fbxbus_rule *name_lost_listener;
	struct wl_listener destroy_listener;
};

static int
publish_hid_service(struct input_lh_udp *input_udp)
{
	struct input_lh *input = input_udp->input;

	if (input_udp->mdns_published) {
		fbxmdnssd_remove(input->bus, input_udp->mdns_id);
		input_udp->mdns_published = false;
	}

	if (!input_udp->mdns_name)
		return 0;

	weston_log("registering hid mDNS service with name %s\n",
		   input_udp->mdns_name);

	if (fbxmdnssd_publish(input->bus, input_udp->mdns_name,
			      UDP_HID_SRV, UDP_HID_PORT,
			      FBXMDNSSD_PROTOCOL_ALL,
			      &input_udp->mdns_id) != FBXMDNSSD_SUCCESS)
		return -1;

	input_udp->mdns_published = true;

	return 0;
}

static int
set_network_name(struct input_lh_udp *input_udp, const char *name)
{
	free(input_udp->mdns_name);
	input_udp->mdns_name = name ? strdup(name) : NULL;

	return publish_hid_service(input_udp);
}

static void
handle_system_name(void *data, const char *name,
		   const char *dns_name,
		   const char *mdns_name,
		   const char *netbios_name)
{
	struct input_lh_udp *input_udp = data;

	set_network_name(input_udp, name);
}

static void
handle_mdnssd_startup(struct fbxbus_msg *msg, void *data)
{
	struct input_lh_udp *input_udp = data;
	const char *srvname;

	srvname = fbxbus_msg_get_str(msg);
	if (!srvname || strcmp(srvname, "fbxmdnssd"))
		return;

	publish_hid_service(input_udp);
}

static void
handle_mdnssd_exit(struct fbxbus_msg *msg, void *data)
{
	struct input_lh_udp *input_udp = data;
	const char *srvname;

	srvname = fbxbus_msg_get_str(msg);
	if (!srvname || strcmp(srvname, "fbxmdnssd"))
		return;

	input_udp->mdns_published = 0;
}

static void
handle_shutdown(struct wl_listener *listener, void *data)
{
	struct input_lh_udp *input_udp =
		container_of(listener, struct input_lh_udp, destroy_listener);
	struct input_lh *input = data;

	fbxsystem_register_name_changed(input->bus, NULL, NULL);

	fbxbus_unregister(input->bus, input_udp->name_acquired_listener);
	fbxbus_unregister(input->bus, input_udp->name_lost_listener);

	set_network_name(input_udp, NULL);

	free(input_udp);
}

int
input_lh_init_udp(struct input_lh *input)
{
	struct input_lh_udp *input_udp;
	struct lh_enumerator *e;
	char *name;

	input_udp = zalloc(sizeof (*input_udp));
	if (!input_udp)
		return -1;

	input_udp->input = input;

	if (lh_rudp_server_create(&input->lh, input->loop, UDP_HID_PORT, &e)) {
		weston_log("failed to create rudp server\n");
		free(input_udp);
		return -1;
	}

	fbxsystem_register_name_changed(input->bus, handle_system_name, input);
	if (!fbxsystem_name_get(input->bus, &name)) {
		set_network_name(input_udp, name);
		free(name);
	}

	input_udp->name_acquired_listener =
		fbxbus_register(input->bus, FBXBUS_SIGNAL,
				FBXBUS_DAEMON_MSG_PATH,
				"name_acquired", handle_mdnssd_startup, input);

	input_udp->name_lost_listener =
		fbxbus_register(input->bus, FBXBUS_SIGNAL,
				FBXBUS_DAEMON_MSG_PATH,
				"name_lost", handle_mdnssd_exit, input);

	input_udp->destroy_listener.notify = handle_shutdown;
	wl_signal_add(&input->destroy_signal, &input_udp->destroy_listener);

	return 0;
}

#else /* ENABLE_LH_UDP */

int
input_lh_init_udp(struct input_lh *input)
{
	return 0;
}

#endif /* !ENABLE_LH_UDP */
