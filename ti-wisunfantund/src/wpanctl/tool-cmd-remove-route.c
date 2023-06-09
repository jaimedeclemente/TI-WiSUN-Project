/*
 *
 * Copyright (c) 2016 Nest Labs, Inc.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *    Description:
 *      This file implements the "remove-route" command in wpanctl
 *
 */

#include <getopt.h>
#include "wpanctl-utils.h"
#include "tool-cmd-remove-route.h"
#include "assert-macros.h"
#include "args.h"
#include "assert-macros.h"
#include "wpan-dbus.h"
#include "string-utils.h"

#include <arpa/inet.h>
#include <errno.h>

const char remove_route_cmd_syntax[] = "[args] <prefix>";

static const arg_list_item_t remove_route_option_list[] = {
	{'h', "help", NULL, "Print Help"},
	{'l', "length", "in bits", "Specify the route prefix length (default is 64)"},
	{'d', "domain", NULL, "Domain id for the route (default is zero)"},
	{0}
};

int tool_cmd_remove_route(int argc, char* argv[])
{
	int ret = 0;
	int c;
	int timeout = DEFAULT_TIMEOUT_IN_SECONDS * 1000;

	DBusConnection* connection = NULL;
	DBusMessage *message = NULL;
	DBusMessage *reply = NULL;
	DBusError error;

	const char *route_prefix = NULL;
	uint8_t prefix_len_in_bits = 64;
	uint16_t domain_id = 0;

	dbus_error_init(&error);

	while (1) {
		static struct option long_options[] = {
			{"help", no_argument, 0, 'h'},
			{"length", required_argument, 0, 'l'},
			{"domain", required_argument, 0, 'd'},
			{0, 0, 0, 0}
		};

		int option_index = 0;
		c = getopt_long(argc, argv, "hl:d:", long_options,
				&option_index);

		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_arg_list_help(remove_route_option_list, argv[0],
					    remove_route_cmd_syntax);
			ret = ERRORCODE_HELP;
			goto bail;

		case 'l' :
			prefix_len_in_bits = (uint8_t) strtol(optarg, NULL, 0);
			break;

		case 'd':
			domain_id = (uint16_t) strtol(optarg, NULL, 0);
			break;
		}
	}

	if (optind < argc) {
		route_prefix = argv[optind];
		optind++;
	} else {
		fprintf((stderr), "%s: No route prefix is given\n", argv[0]);
		ret = ERRORCODE_BADARG;
		goto bail;
	}

	if (optind < argc) {
		fprintf(stderr,
		        "%s: error: Unexpected extra argument: \"%s\"\n",
		        argv[0], argv[optind]);
		ret = ERRORCODE_BADARG;
		goto bail;
	}

	if (gInterfaceName[0] == 0) {
		fprintf(stderr,
		        "%s: error: No WPAN interface set (use the `cd` command, or the `-I` argument for `wpanctl`).\n",
		        argv[0]);
		ret = ERRORCODE_BADARG;
		goto bail;
	}

	connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

	require_string(connection != NULL, bail, error.message);

	{
		DBusMessageIter iter;
		DBusMessageIter list_iter;
		char path[DBUS_MAXIMUM_NAME_LENGTH+1];
		char interface_dbus_name[DBUS_MAXIMUM_NAME_LENGTH+1];
		ret = lookup_dbus_name_from_interface(interface_dbus_name, gInterfaceName);
		if (ret != 0) {
			goto bail;
		}
		snprintf(path,
		         sizeof(path),
		         "%s/%s",
		         WPAN_TUNNEL_DBUS_PATH,
		         gInterfaceName);

		message = dbus_message_new_method_call(
		    interface_dbus_name,
		    path,
		    WPAN_TUNNEL_DBUS_INTERFACE,
		    WPANTUND_IF_CMD_ROUTE_REMOVE
		);

		if ((route_prefix != NULL) && (prefix_len_in_bits <= 128)) {
			uint8_t prefix_bytes[16];

			memset(prefix_bytes, 0, sizeof(prefix_bytes));

			// So the prefix could either be
			// specified like an IPv6 address, or
			// specified as a bunch of hex numbers.
			// We use the presence of a colon (':')
			// to differentiate.
			if (strstr(route_prefix, ":")) {

				// Address-style
				int bits = inet_pton(AF_INET6, route_prefix, prefix_bytes);
				if (bits < 0) {
					fprintf(stderr,
					        "Bad prefix \"%s\", errno=%d (%s)\n",
					        route_prefix,
					        errno,
					        strerror(errno));
					goto bail;
				} else if (bits == 0) {
					fprintf(stderr, "Bad prefix \"%s\"\n", route_prefix);
					goto bail;
				}
			} else {
				// DATA-style
				int length = parse_string_into_data(prefix_bytes,
				                                    16,
				                                    route_prefix);
				if(length<=0) {
					fprintf(stderr, "Bad prefix \"%s\"\n", route_prefix);
					goto bail;
				}
			}

			fprintf(stderr, "Removing route prefix \"%s\" with len %d, domain-id %d.\n",
				route_prefix, prefix_len_in_bits, domain_id
			);

			uint8_t *addr = prefix_bytes;
			uint8_t len = (prefix_len_in_bits + 7) / 8;

			dbus_message_append_args(
			    message,
			    DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &addr, len,
			    DBUS_TYPE_INVALID
			);
		} else {
			dbus_message_append_args(
			    message,
			    DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, "", 0,
			    DBUS_TYPE_INVALID
			);
		}

		dbus_message_append_args(
		    message,
		    DBUS_TYPE_UINT16, &domain_id,
		    DBUS_TYPE_BYTE, &prefix_len_in_bits,
		    DBUS_TYPE_INVALID
		);

		reply = dbus_connection_send_with_reply_and_block(
		    connection,
		    message,
		    timeout,
		    &error
		);

		if (!reply) {
			fprintf(stderr, "%s: error: %s\n", argv[0], error.message);
			ret = ERRORCODE_TIMEOUT;
			goto bail;
		}

		dbus_message_get_args(reply, &error,
		                      DBUS_TYPE_INT32, &ret,
		                      DBUS_TYPE_INVALID
		                      );

		if (!ret) {
			fprintf(stderr, "Route prefix removed.\n");
		} else {
			fprintf(stderr, "%s failed with error %d. %s\n", argv[0], ret, wpantund_status_to_cstr(ret));
			print_error_diagnosis(ret);
		}
	}

bail:

	if (connection)
		dbus_connection_unref(connection);

	if (message)
		dbus_message_unref(message);

	if (reply)
		dbus_message_unref(reply);

	dbus_error_free(&error);

	return ret;
}
