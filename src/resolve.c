/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2019  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>

#include <ell/ell.h>

#include "src/iwd.h"
#include "src/dbus.h"
#include "src/resolve.h"

struct resolve_method_ops {
	void *(*init)(void);
	void (*exit)(void *data);
	void (*add_dns)(uint32_t ifindex, uint8_t type, char **dns_list,
								void *data);
	void (*remove)(uint32_t ifindex, void *data);
};

struct resolve_method {
	void *data;
	const struct resolve_method_ops *ops;
};

static struct resolve_method method;

#define SYSTEMD_RESOLVED_SERVICE           "org.freedesktop.resolve1"

struct systemd_state {
	uint32_t service_watch;
	bool is_ready:1;
};

static void resolve_systemd_add_dns(uint32_t ifindex, char **dns_list,
								void *data)
{
	struct systemd_state *state = data;

	l_debug("ifindex: %u", ifindex);

	if (!state->is_ready) {
		l_error("resolve-systemd: Failed to add DNS entries. "
				"Is 'systemd-resolved' service running?");

		return;
	}

	/* TODO */
}

static void resolve_systemd_remove(uint32_t ifindex, void *data)
{
	struct systemd_state *state = data;

	l_debug("ifindex: %u", ifindex);

	if (!state->is_ready) {
		l_error("resolve-systemd: Failed to remove DNS entries. "
				"Is 'systemd-resolved' service running?");

		return;
	}

	/* TODO */
}

static void systemd_appeared(struct l_dbus *dbus, void *user_data)
{
	struct systemd_state *state = user_data;

	state->is_ready = true;
}

static void systemd_disappeared(struct l_dbus *dbus, void *user_data)
{
	struct systemd_state *state = user_data;

	state->is_ready = false;
}

static void *resolve_systemd_init(void)
{
	struct systemd_state *state;

	state = l_new(struct systemd_state, 1);

	state->service_watch =
		l_dbus_add_service_watch(dbus_get_bus(),
						SYSTEMD_RESOLVED_SERVICE,
						systemd_appeared,
						systemd_disappeared,
						state, NULL);

	return state;
}

static void resolve_systemd_exit(void *data)
{
	struct systemd_state *state = data;

	l_dbus_remove_watch(dbus_get_bus(), state->service_watch);

	l_free(state);
}

static const struct resolve_method_ops resolve_method_systemd = {
	.init = resolve_systemd_init,
	.exit = resolve_systemd_exit,
	.add_dns = resolve_systemd_add_dns,
	.remove = resolve_systemd_remove,
};

void resolve_add_dns(uint32_t ifindex, uint8_t type, char **dns_list)
{
	if (!dns_list || !*dns_list)
		return;

	if (!method.ops || !method.ops->add_dns)
		return;

	method.ops->add_dns(ifindex, type, dns_list, method.data);
}

void resolve_remove(uint32_t ifindex)
{
	if (!method.ops || !method.ops->remove)
		return;

	method.ops->remove(ifindex, method.data);
}

static const struct {
	const char *name;
	const struct resolve_method_ops *method_ops;
} resolve_method_ops_list[] = {
	{ "systemd", &resolve_method_systemd },
	{ }
};

static int resolve_init(void)
{
	const char *method_name;
	bool enabled;
	uint8_t i;

	if (!l_settings_get_bool(iwd_get_config(), "General",
					"enable_network_config", &enabled) ||
								!enabled)
		return 0;

	method_name = l_settings_get_value(iwd_get_config(), "General",
							"dns_resolve_method");

	if (!method_name)
		/* Default to systemd-resolved service. */
		method_name = "systemd";

	for (i = 0; resolve_method_ops_list[i].name; i++) {
		if (strcmp(resolve_method_ops_list[i].name, method_name))
			continue;

		method.ops = resolve_method_ops_list[i].method_ops;

		break;
	}

	if (!method.ops)
		return -EINVAL;

	if (method.ops->init)
		method.data = method.ops->init();

	return 0;
}

static void resolve_exit(void)
{
	if (!method.ops->exit)
		return;

	method.ops->exit(method.data);
}

IWD_MODULE(resolve, resolve_init, resolve_exit)