/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <ell/ell.h>

#include "src/netdev.h"
#include "src/wiphy.h"
#include "src/kdbus.h"
#include "src/dbus.h"
#include "src/agent.h"

static struct l_timeout *timeout = NULL;

static void main_loop_quit(struct l_timeout *timeout, void *user_data)
{
	l_main_quit();
}

static void signal_handler(struct l_signal *signal, uint32_t signo,
							void *user_data)
{
	switch (signo) {
	case SIGINT:
	case SIGTERM:
		l_info("Terminate");

		agent_exit();

		timeout = l_timeout_create(1, main_loop_quit, NULL, NULL);
		break;
	}
}

static void usage(void)
{
	printf("iwd - Wireless daemon\n"
		"Usage:\n");
	printf("\tiwd [options]\n");
	printf("Options:\n"
		"\t-B, --dbus-debug       Enable DBus debugging\n"
		"\t-K, --kdbus            Setup Kernel D-Bus\n"
		"\t-h, --help             Show help options\n");
}

static const struct option main_options[] = {
	{ "kdbus",      no_argument,       NULL, 'K' },
	{ "dbus-debug", no_argument,       NULL, 'B' },
	{ "version",    no_argument,       NULL, 'v' },
	{ "help",       no_argument,       NULL, 'h' },
	{ }
};

int main(int argc, char *argv[])
{
	bool enable_kdbus = false;
	bool enable_dbus_debug = false;
	struct l_signal *signal;
	sigset_t mask;
	int exit_status;

	for (;;) {
		int opt;

		opt = getopt_long(argc, argv, "Bvh", main_options, NULL);
		if (opt < 0)
			break;

		switch (opt) {
		case 'K':
			enable_kdbus = true;
			break;
		case 'B':
			enable_dbus_debug = true;
			break;
		case 'v':
			printf("%s\n", VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage();
			return EXIT_SUCCESS;
		default:
			return EXIT_FAILURE;
		}
	}

	if (argc - optind > 0) {
		fprintf(stderr, "Invalid command line parameters\n");
		return EXIT_FAILURE;
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	signal = l_signal_create(&mask, signal_handler, NULL, NULL);

	l_log_set_stderr();
	l_debug_enable("*");

	l_info("Wireless daemon version %s", VERSION);

	if (enable_kdbus) {
		char *bus_name;
		bool result;

		if (!kdbus_create_bus()) {
			exit_status = EXIT_FAILURE;
			goto done;
		}

		bus_name = kdbus_lookup_bus();
		if (!bus_name) {
			exit_status = EXIT_FAILURE;
			goto destroy;
		}

		l_debug("Bus location: %s", bus_name);

		result = kdbus_open_bus(bus_name, "net.connman.iwd", "iwd");

		l_free(bus_name);

		if (!result) {
			exit_status = EXIT_FAILURE;
			goto destroy;
		}
	}

	if (!dbus_init(enable_dbus_debug)) {
		exit_status = EXIT_FAILURE;
		goto destroy;
	}

	if (!netdev_init()) {
		exit_status = EXIT_FAILURE;
		goto destroy;
	}

	if (!wiphy_init()) {
		netdev_exit();
		exit_status = EXIT_FAILURE;
		goto destroy;
	}

	l_main_run();

	wiphy_exit();
	netdev_exit();
	dbus_exit();

	exit_status = EXIT_SUCCESS;

destroy:
	if (enable_kdbus)
		kdbus_destroy_bus();

done:
	l_signal_remove(signal);
	l_timeout_remove(timeout);

	return exit_status;
}
