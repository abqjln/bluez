/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2005  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
 *  CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
 *  COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
 *  SOFTWARE IS DISCLAIMED.
 *
 *
 *  $Id$
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syslog.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <dbus/dbus.h>

#include "glib-ectomy.h"

#include "hcid.h"
#include "dbus.h"

static DBusConnection *connection;
static int num_adapters = 0;

#define TIMEOUT (30 * 1000)		/* 30 seconds */
#define BLUETOOTH_DEVICE_NAME_LEN    (18)
#define BLUETOOTH_DEVICE_ADDR_LEN    (18)
#define MAX_PATH_LENGTH   (64)
#define READ_REMOTE_NAME_TIMEOUT	(25000)
#define MAX_CONN_NUMBER			(10)

#define PINAGENT_SERVICE_NAME BASE_INTERFACE ".PinAgent"
#define PINAGENT_INTERFACE PINAGENT_SERVICE_NAME
#define PIN_REQUEST "PinRequest"
#define PINAGENT_PATH BASE_PATH "/PinAgent"

struct pin_request {
	int dev;
	bdaddr_t bda;
};

typedef DBusMessage* (service_handler_func_t)(DBusMessage *, void *);

struct service_data {
	const char             *name;
	service_handler_func_t *handler_func;
	const char             *signature;
};

struct hci_dbus_data {
	uint16_t id;
};

typedef int register_function_t(DBusConnection *conn, int dft_reg, uint16_t id);
typedef int unregister_function_t(DBusConnection *conn, int unreg_dft, uint16_t id);

const struct service_data *get_hci_table(void);

static int hci_dbus_reg_obj_path(DBusConnection *conn, int dft_reg, uint16_t id);
static int hci_dbus_unreg_obj_path(DBusConnection *conn, int unreg_dft, uint16_t id);

typedef const struct service_data *get_svc_table_func_t(void);

struct profile_obj_path_data {
	const char		*name;
	int			status; /* 1:active  0:disabled */
	int			dft_reg; /* dft path registered */
	register_function_t     *reg_func;
	unregister_function_t   *unreg_func;
	get_svc_table_func_t *get_svc_table; /* return the service table */
};

/*
 * D-Bus error messages functions and declarations.
 * This section should be moved to a common file 
 * in the future
 *
 */
typedef struct  {
	uint32_t code;
	const char *str;
}bluez_error_t;

static const bluez_error_t error_array[] = {
	{ BLUEZ_EDBUS_UNKNOWN_METHOD,	"Method not found"		},
	{ BLUEZ_EDBUS_WRONG_SIGNATURE,	"Wrong method signature"	},
	{ BLUEZ_EDBUS_WRONG_PARAM,	"Invalid parameters"		},
	{ BLUEZ_EDBUS_RECORD_NOT_FOUND,	"No record found"		},
	{ BLUEZ_EDBUS_NO_MEM,		"No memory"			},
	{ BLUEZ_EDBUS_CONN_NOT_FOUND,	"Connection not found"		},
	{ BLUEZ_EDBUS_UNKNOWN_PATH,	"Device path is not registered"	},
	{ 0,				NULL }
};

static const char *bluez_dbus_error_to_str(const uint32_t ecode) 
{
	const bluez_error_t *ptr;
	uint32_t raw_code = 0;

	if (ecode & BLUEZ_ESYSTEM_OFFSET) {
		/* System error */
		raw_code = (~BLUEZ_ESYSTEM_OFFSET) & ecode;
		syslog(LOG_INFO, "%s - msg:%s", __PRETTY_FUNCTION__, strerror(raw_code));
		return strerror(raw_code);
	} else if (ecode & BLUEZ_EDBUS_OFFSET) { 
		/* D-Bus error */
		for (ptr = error_array; ptr->code; ptr++) {
			if (ptr->code == ecode) {
				syslog(LOG_INFO, "%s - msg:%s", __PRETTY_FUNCTION__, ptr->str);
				return ptr->str;
			}
		}
	}

	return NULL;
}

static DBusMessage *bluez_new_failure_msg(DBusMessage *msg, const uint32_t ecode)
{
	DBusMessageIter iter;
	DBusMessage *reply = NULL;
	const char *error_msg = NULL;

	error_msg = bluez_dbus_error_to_str(ecode);

	if (error_msg) {
		reply = dbus_message_new_error(msg, ERROR_INTERFACE, error_msg);

		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32 ,&ecode);
	}

	return reply;
}

/*
 * Object path register/unregister functions 
 *
 */
static struct profile_obj_path_data obj_path_table[] = {
	{ BLUEZ_HCI, 1, 0, hci_dbus_reg_obj_path, hci_dbus_unreg_obj_path, get_hci_table },
	/* add other profiles here */
	{ NULL, 0, 0, NULL, NULL, NULL }
};

/*
 * Device Message handler functions object table declaration
 */
static DBusHandlerResult msg_func(DBusConnection *conn, DBusMessage *msg, void *data);

static DBusMessage* handle_get_devices_req(DBusMessage *msg, void *data);
static DBusMessage* handle_not_implemented_req(DBusMessage *msg, void *data);

static const DBusObjectPathVTable obj_vtable = {
	.message_function = &msg_func,
	.unregister_function = NULL
};

/*
 * Service provided under the path DEVICE_PATH
 * TODO add the handlers
 */
static const struct service_data dev_services[] = {
	{ DEV_UP,		handle_not_implemented_req,	DEV_UP_SIGNATURE		},
	{ DEV_DOWN,		handle_not_implemented_req,	DEV_DOWN_SIGNATURE		},
	{ DEV_RESET,		handle_not_implemented_req,	DEV_RESET_SIGNATURE		},
	{ DEV_SET_PROPERTY,	handle_not_implemented_req,	DEV_SET_PROPERTY_SIGNATURE	},
	{ DEV_GET_PROPERTY,	handle_not_implemented_req,	DEV_GET_PROPERTY_SIGNATURE	},
	{ NULL, NULL, NULL}
};

/*
 * Manager Message handler functions object table declaration
 *
 */
static const struct service_data mgr_services[] = {
	{ MGR_GET_DEV,		handle_get_devices_req,		MGR_GET_DEV_SIGNATURE	},
	{ MGR_INIT,		handle_not_implemented_req,	NULL			},
	{ MGR_ENABLE,		handle_not_implemented_req,	NULL			},
	{ MGR_DISABLE,		handle_not_implemented_req,	NULL			},
	{ NULL,			handle_not_implemented_req,	NULL			}
};

/*
 * HCI Manager Message handler functions object table declaration
 *
 */
static DBusHandlerResult hci_signal_filter (DBusConnection *conn, DBusMessage *msg, void *data);

static DBusMessage* handle_periodic_inq_req(DBusMessage *msg, void *data);
static DBusMessage* handle_cancel_periodic_inq_req(DBusMessage *msg, void *data);
static DBusMessage* handle_inq_req(DBusMessage *msg, void *data);
static DBusMessage* handle_role_switch_req(DBusMessage *msg, void *data);
static DBusMessage* handle_remote_name_req(DBusMessage *msg, void *data);
static DBusMessage* handle_display_conn_req(DBusMessage *msg, void *data);

static const struct service_data hci_services[] = {
	{ HCI_PERIODIC_INQ,		handle_periodic_inq_req,	HCI_PERIODIC_INQ_SIGNATURE		},
	{ HCI_CANCEL_PERIODIC_INQ,	handle_cancel_periodic_inq_req,	HCI_CANCEL_PERIODIC_INQ_SIGNATURE	},
	{ HCI_ROLE_SWITCH,		handle_role_switch_req,		HCI_ROLE_SWITCH_SIGNATURE		},
	{ HCI_INQ,			handle_inq_req,			HCI_INQ_SIGNATURE			},
	{ HCI_REMOTE_NAME,		handle_remote_name_req,		HCI_REMOTE_NAME_SIGNATURE		},
	{ HCI_CONNECTIONS,		handle_display_conn_req,	HCI_CONNECTIONS_SIGNATURE		},
	{ NULL,				NULL,				NULL					}
};

static void reply_handler_function(DBusPendingCall *call, void *user_data)
{
	struct pin_request *req = (struct pin_request *) user_data;
	pin_code_reply_cp pr;
	DBusMessage *message;
	DBusMessageIter iter;
	int arg_type;
	int msg_type;
	size_t len;
	char *pin;
	const char *error_msg;

	message = dbus_pending_call_steal_reply(call);

	if (message) {
		msg_type = dbus_message_get_type(message);
		dbus_message_iter_init(message, &iter);
		
		if (msg_type == DBUS_MESSAGE_TYPE_ERROR) {
			dbus_message_iter_get_basic(&iter, &error_msg);

			/* handling WRONG_ARGS_ERROR, DBUS_ERROR_NO_REPLY, DBUS_ERROR_SERVICE_UNKNOWN */
			syslog(LOG_ERR, "%s: %s", dbus_message_get_error_name(message), error_msg);
			hci_send_cmd(req->dev, OGF_LINK_CTL,
					OCF_PIN_CODE_NEG_REPLY, 6, &req->bda);
		} else {
			/* check signature */
			arg_type = dbus_message_iter_get_arg_type(&iter);
			if (arg_type != DBUS_TYPE_STRING) {
				syslog(LOG_ERR, "Wrong reply signature: expected PIN");
				hci_send_cmd(req->dev, OGF_LINK_CTL,
						OCF_PIN_CODE_NEG_REPLY, 6, &req->bda);
			} else {
				dbus_message_iter_get_basic(&iter, &pin);
				len = strlen(pin);

				memset(&pr, 0, sizeof(pr));
				bacpy(&pr.bdaddr, &req->bda);
				memcpy(pr.pin_code, pin, len);
				pr.pin_len = len;
				hci_send_cmd(req->dev, OGF_LINK_CTL, OCF_PIN_CODE_REPLY,
						PIN_CODE_REPLY_CP_SIZE, &pr);
			}
		}

		dbus_message_unref(message);
	}

	dbus_pending_call_unref(call);
}

static void free_pin_req(void *req)
{
	free(req);
}

void hcid_dbus_request_pin(int dev, struct hci_conn_info *ci)
{
	DBusMessage *message;
	DBusPendingCall *pending = NULL;
	struct pin_request *req;
	uint8_t *addr = (uint8_t *) &ci->bdaddr;
	dbus_bool_t out = ci->out;

	message = dbus_message_new_method_call(PINAGENT_SERVICE_NAME, PINAGENT_PATH,
						PINAGENT_INTERFACE, PIN_REQUEST);
	if (message == NULL) {
		syslog(LOG_ERR, "Couldn't allocate D-BUS message");
		goto failed;
	}

	req = malloc(sizeof(*req));
	req->dev = dev;
	bacpy(&req->bda, &ci->bdaddr);

	dbus_message_append_args(message, DBUS_TYPE_BOOLEAN, &out,
			DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			&addr, sizeof(bdaddr_t), DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, message,
					&pending, TIMEOUT) == FALSE) {
		syslog(LOG_ERR, "D-BUS send failed");
		goto failed;
	}

	dbus_pending_call_set_notify(pending, reply_handler_function,
							req, free_pin_req);

	dbus_connection_flush(connection);

	dbus_message_unref(message);

	return;

failed:
	dbus_message_unref(message);
	hci_send_cmd(dev, OGF_LINK_CTL,
				OCF_PIN_CODE_NEG_REPLY, 6, &ci->bdaddr);
}

void hcid_dbus_inquiry_start(bdaddr_t *local)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d/%s", MANAGER_PATH, id, BLUEZ_HCI);

	message = dbus_message_new_signal(path,
				BLUEZ_HCI_INTERFACE, BLUEZ_HCI_INQ_START);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry start message");
		goto failed;
	}

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry start message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);

	return;
}

void hcid_dbus_inquiry_complete(bdaddr_t *local)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d/%s", MANAGER_PATH, id, BLUEZ_HCI);

	message = dbus_message_new_signal(path,
				BLUEZ_HCI_INTERFACE, BLUEZ_HCI_INQ_COMPLETE);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry complete message");
		goto failed;
	}

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry complete message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);

	return;
}

void hcid_dbus_inquiry_result(bdaddr_t *local, bdaddr_t *peer, uint32_t class, int8_t rssi)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr, *peer_addr;
	dbus_uint32_t tmp_class = class;
	dbus_int32_t tmp_rssi = rssi;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d/%s", MANAGER_PATH, id, BLUEZ_HCI);

	message = dbus_message_new_signal(path,
				BLUEZ_HCI_INTERFACE, BLUEZ_HCI_INQ_RESULT);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry result message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_UINT32, &tmp_class,
					DBUS_TYPE_INT32, &tmp_rssi,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry result message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);

	return;
}

void hcid_dbus_remote_name(bdaddr_t *local, bdaddr_t *peer, char *name)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr, *peer_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d/%s", MANAGER_PATH, id, BLUEZ_HCI);

	message = dbus_message_new_signal(path,
				BLUEZ_HCI_INTERFACE, BLUEZ_HCI_REMOTE_NAME);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_STRING, &name,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS remote name message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);

	return;
}

void hcid_dbus_conn_complete(bdaddr_t *local, bdaddr_t *peer)
{
}

void hcid_dbus_disconn_complete(bdaddr_t *local, bdaddr_t *peer, uint8_t reason)
{
}

gboolean watch_func(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	DBusWatch *watch = (DBusWatch *) data;
	int flags = 0;

	if (cond & G_IO_IN)  flags |= DBUS_WATCH_READABLE;
	if (cond & G_IO_OUT) flags |= DBUS_WATCH_WRITABLE;
	if (cond & G_IO_HUP) flags |= DBUS_WATCH_HANGUP;
	if (cond & G_IO_ERR) flags |= DBUS_WATCH_ERROR;

	dbus_watch_handle(watch, flags);

	dbus_connection_ref(connection);

	/* Dispatch messages */
	while (dbus_connection_dispatch(connection) == DBUS_DISPATCH_DATA_REMAINS);

	dbus_connection_unref(connection);

	return TRUE;
}

dbus_bool_t add_watch(DBusWatch *watch, void *data)
{
	GIOCondition cond = G_IO_HUP | G_IO_ERR;
	GIOChannel *io;
	guint *id;
	int fd, flags;

	if (!dbus_watch_get_enabled(watch))
		return TRUE;

	id = malloc(sizeof(guint));
	if (id == NULL)
		return FALSE;

	fd = dbus_watch_get_fd(watch);
	io = g_io_channel_unix_new(fd);
	flags = dbus_watch_get_flags(watch);

	if (flags & DBUS_WATCH_READABLE) cond |= G_IO_IN;
	if (flags & DBUS_WATCH_WRITABLE) cond |= G_IO_OUT;

	*id = g_io_add_watch(io, cond, watch_func, watch);

	dbus_watch_set_data(watch, id, NULL);

	return TRUE;
}

static void remove_watch(DBusWatch *watch, void *data)
{
	guint *id = dbus_watch_get_data(watch);

	dbus_watch_set_data(watch, NULL, NULL);

	if (id) {
		g_io_remove_watch(*id);
		free(id);
	}
}

static void watch_toggled(DBusWatch *watch, void *data)
{
	/* Because we just exit on OOM, enable/disable is
	 * no different from add/remove */
	if (dbus_watch_get_enabled(watch))
		add_watch(watch, data);
	else
		remove_watch(watch, data);
}

gboolean hcid_dbus_init(void)
{
	struct hci_dbus_data *data;
	DBusError error;

	dbus_error_init(&error);

	connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

	if (dbus_error_is_set(&error)) {
		syslog(LOG_ERR, "Can't open system message bus connection: %s\n",
								error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	dbus_bus_request_name(connection, BASE_INTERFACE,
				DBUS_NAME_FLAG_PROHIBIT_REPLACEMENT, &error);

	if (dbus_error_is_set(&error)) {
		syslog(LOG_ERR,"Can't get system message bus name: %s\n",
								error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	data = malloc(sizeof(struct hci_dbus_data));
	if (data == NULL)
		return FALSE;

	data->id = DEVICE_PATH_ID;

	if (!dbus_connection_register_object_path(connection, DEVICE_PATH,
						&obj_vtable, data)) {
		syslog(LOG_ERR, "Can't register %s object", DEVICE_PATH);
		return FALSE;
	}

	data = malloc(sizeof(struct hci_dbus_data));
	if (data == NULL)
		return FALSE;

	data->id = MANAGER_PATH_ID;

	if (!dbus_connection_register_fallback(connection, MANAGER_PATH,
						&obj_vtable, data)) {
		syslog(LOG_ERR, "Can't register %s object", MANAGER_PATH);
		return FALSE;
	}

	if (!dbus_connection_add_filter(connection, hci_signal_filter, NULL, NULL)) {
		syslog(LOG_ERR, "Can't add new HCI filter");
		return FALSE;
	}

	dbus_connection_set_watch_functions(connection,
			add_watch, remove_watch, watch_toggled, NULL, NULL);

	return TRUE;
}

void hcid_dbus_exit(void)
{
	char path[MAX_PATH_LENGTH];
	char fst_parent[] = MANAGER_PATH;
	char snd_parent[MAX_PATH_LENGTH];
	char **fst_level = NULL;
	char **snd_level = NULL;
	char *ptr1;
	char *ptr2;
	void *data = NULL;

	if (!connection)
		return;

	if (dbus_connection_get_object_path_data(connection,
				DEVICE_PATH, &data)) {
		if (data) {
			free(data);
			data = NULL;
		}
	}

	if (!dbus_connection_unregister_object_path(connection, DEVICE_PATH))
		syslog(LOG_ERR, "Can't unregister %s object", DEVICE_PATH);

	if (dbus_connection_get_object_path_data(connection,
				MANAGER_PATH, &data)) {
		if (data) {
			free(data);
			data = NULL;
		}
	}

	if (!dbus_connection_unregister_object_path(connection, MANAGER_PATH))
		syslog(LOG_ERR, "Can't unregister %s object", MANAGER_PATH);

	if (dbus_connection_list_registered(connection, fst_parent, &fst_level)) {

		for (; *fst_level; fst_level++) {
			ptr1 = *fst_level;
			sprintf(snd_parent, "%s/%s", fst_parent, ptr1);

			if (dbus_connection_list_registered(connection, snd_parent, &snd_level)) {

				if (!(*snd_level)) {
					sprintf(path, "%s/%s", MANAGER_PATH, ptr1);

					if (dbus_connection_get_object_path_data(connection,
								path, &data)) {
						if (data) {
							free(data);
							data = NULL;
						}
					}

					if (!dbus_connection_unregister_object_path(connection, path))
						syslog(LOG_ERR, "Can't unregister %s object", path);

					continue;
				}

				for (; *snd_level; snd_level++) {
					ptr2 = *snd_level;
					sprintf(path, "%s/%s/%s", MANAGER_PATH, ptr1, ptr2);

					if (dbus_connection_get_object_path_data(connection,
								path, &data)) {
						if (data) {
							free(data);
							data = NULL;
						}
					}

					if (!dbus_connection_unregister_object_path(connection, path))
						syslog(LOG_ERR, "Can't unregister %s object", path);
				}

				if (*snd_level)
					dbus_free_string_array(snd_level);
			}
		}

		if (*fst_level)
			dbus_free_string_array(fst_level);
	}
}

gboolean hcid_dbus_register_device(uint16_t id)
{
	struct profile_obj_path_data *ptr = obj_path_table;
	int ret = -1; 

	if (!connection)
		return FALSE;

	for (; ptr->name; ptr++) {
		ret = ptr->reg_func(connection, ptr->dft_reg, id);
		ptr->dft_reg = 1;
	}

	if (!ret)
		num_adapters++;

	return TRUE;
}

gboolean hcid_dbus_unregister_device(uint16_t id)
{
	struct profile_obj_path_data *ptr = obj_path_table;
	int dft_unreg = 0;

	if (!connection)
		return FALSE;

	for (; ptr->name; ptr++) {
		dft_unreg = (num_adapters > 1) ? 0 : 1;
		num_adapters--;
		ptr->unreg_func(connection, dft_unreg, id);

		if (dft_unreg )
			ptr->dft_reg = 0;
	}

	return TRUE;
}

/*
 * @brief HCI object path register function
 * Detailed description: function responsible for register a new hci 
 * D-Bus path. If necessary the default path must be registered too.
 * @param conn D-Bus connection
 * @param dft_reg register the default path(0 or !0)
 * @param id hci device identification
 * @return (0-Success/-1 failure)
 */
static int hci_dbus_reg_obj_path(DBusConnection *conn, int dft_reg, uint16_t id)
{
	struct hci_dbus_data *data;
	char path[MAX_PATH_LENGTH];

	/* register the default path*/
	if (!dft_reg) {

		sprintf(path, "%s/%s/%s", MANAGER_PATH, HCI_DEFAULT_DEVICE_NAME, BLUEZ_HCI);

		data = malloc(sizeof(struct hci_dbus_data));
		if (data == NULL)
			return -1;

		data->id = DEFAULT_DEVICE_PATH_ID;

		if (!dbus_connection_register_object_path(conn, path, &obj_vtable, data)) { 
			syslog(LOG_ERR,"DBUS failed to register %s object", path);
			/* ignore, the default path was already registered */
		}
	}

	data = malloc(sizeof(struct hci_dbus_data));
	if (data == NULL)
		return -1;

	data->id = id;

	/* register the default path*/
	sprintf(path, "%s/%s%d/%s", MANAGER_PATH, HCI_DEVICE_NAME, id, BLUEZ_HCI);

	if (!dbus_connection_register_object_path(conn, path, &obj_vtable, data)) {
		syslog(LOG_ERR,"DBUS failed to register %s object", path);
		/* ignore, the path was already registered */
	}

	return 0;
}

/*
 * @brief HCI object path unregister function
 * Detailed description: function responsible for unregister HCI D-Bus
 * path for a detached hci device. If necessary the default path must 
 * be registered too.
 * @param conn D-Bus connection
 * @param unreg_dft register the default path(0 or !0)
 * @param id hci device identification
 * @return (0-Success/-1 failure)
 */
static int hci_dbus_unreg_obj_path(DBusConnection *conn, int unreg_dft, uint16_t id) 
{
	int ret = 0;
	char path[MAX_PATH_LENGTH];
	char dft_path[MAX_PATH_LENGTH];
	void *data = NULL;

	if (unreg_dft) {

		sprintf(dft_path, "%s/%s/%s", MANAGER_PATH, HCI_DEFAULT_DEVICE_NAME, BLUEZ_HCI);

		if (!dbus_connection_unregister_object_path (connection, dft_path)) {
			syslog(LOG_ERR,"DBUS failed to unregister %s object", dft_path);
			ret = -1;
		} else {
			if (dbus_connection_get_object_path_data(conn, dft_path, &data)) {
				if (data) {
					free(data);
					data = NULL;
				}
			}
		}
	}

	sprintf(path, "%s/%s%d/%s", MANAGER_PATH, HCI_DEVICE_NAME, id, BLUEZ_HCI);

	if (!dbus_connection_unregister_object_path (connection, path)) {
		syslog(LOG_ERR,"DBUS failed to unregister %s object", path);
		ret = -1;
	} else {
		if (dbus_connection_get_object_path_data(conn, path, &data)) {
			if (data) {
				free(data);
				data = NULL;
			}
		}
	}

	return ret;
}

const struct service_data *get_hci_table(void)
{
	return hci_services;
}

/*****************************************************************
 *  
 *  Section reserved to HCI Manaher D-Bus message handlers
 *  
 *****************************************************************/

static DBusHandlerResult hci_signal_filter (DBusConnection *conn, DBusMessage *msg, void *data)
{
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *iface;
	const char *method;

	if (!msg || !conn)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_get_type (msg) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	iface = dbus_message_get_interface(msg);
	method = dbus_message_get_member(msg);

	if (strcmp(iface, DBUS_INTERFACE_LOCAL) == 0) {
		if (strcmp(method, "Disconnected") == 0)
			ret = DBUS_HANDLER_RESULT_HANDLED;
	} else if (strcmp(iface, DBUS_INTERFACE_DBUS) == 0) {
		if (strcmp(method, "NameOwnerChanged") == 0)
			ret = DBUS_HANDLER_RESULT_HANDLED;

		if (strcmp(method, "NameAcquired") == 0)
			ret = DBUS_HANDLER_RESULT_HANDLED;
	}

	return ret;
}
/*
 * There is only one message handler function for all object paths
 *
 */

static DBusHandlerResult msg_func(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const struct service_data *ptr_handlers = NULL;
	DBusMessage *reply = NULL;
	int type;
	const char *iface;
	const char *method;
	const char *signature;
	const char *path;
	const char *rel_path;
	const char *tmp_iface = NULL;
	struct hci_dbus_data *dbus_data = data;
	uint32_t result = BLUEZ_EDBUS_UNKNOWN_METHOD;
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	uint8_t found = 0;

	path = dbus_message_get_path(msg);
	type = dbus_message_get_type(msg);
	iface = dbus_message_get_interface(msg);
	method = dbus_message_get_member (msg);
	signature = dbus_message_get_signature(msg);

	syslog (LOG_INFO, "%s - path:%s, id:0x%X", __PRETTY_FUNCTION__, path, dbus_data->id);

	if (strcmp(path, DEVICE_PATH) == 0) {
		ptr_handlers = dev_services;
		tmp_iface = DEVICE_INTERFACE;
		found = 1;
	} else {
		if (strcmp(path, MANAGER_PATH) > 0) {
			/* it is device specific path */
			if ( dbus_data->id == MANAGER_PATH_ID ) {
				/* fallback handling. The child path IS NOT registered */
				reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_UNKNOWN_PATH);
				ret = DBUS_HANDLER_RESULT_HANDLED;
			} else {
				const struct profile_obj_path_data *mgr_child = obj_path_table;
				rel_path = strrchr(path,'/');
				rel_path++;

				if (rel_path) {
					for ( ;mgr_child->name; mgr_child++) {
						if (strcmp(mgr_child->name, rel_path) == 0) {
							ptr_handlers = mgr_child->get_svc_table();
							found = 1;
						}
					}

					tmp_iface = MANAGER_INTERFACE;
				}
			}
		} else {
			/* it's the manager path */
			ptr_handlers = mgr_services;
			tmp_iface = MANAGER_INTERFACE;
			found = 1;
		}
	}

	if (found && (type == DBUS_MESSAGE_TYPE_METHOD_CALL) && 
		(strcmp(iface, tmp_iface) == 0) && (method != NULL)) {

		for (; ptr_handlers->name; ptr_handlers++) {
			if (strcmp(method, ptr_handlers->name) == 0) {
				/* resetting unknown method. It's possible handle method overload */
				result = BLUEZ_EDBUS_WRONG_SIGNATURE; 
				if (strcmp(ptr_handlers->signature, signature) == 0) {
					if (ptr_handlers->handler_func) {
						reply = (ptr_handlers->handler_func)(msg, data);
						result = 0; /* resetting wrong signature*/
					} else 
						syslog(LOG_INFO, "Service not implemented");

					break;
				} 
				
			}
		}

		if (result) {
			reply = bluez_new_failure_msg(msg, result);
		}

		ret = DBUS_HANDLER_RESULT_HANDLED;
	}
	
	/* send an error or the success reply*/
	if (reply) {
		if (!dbus_connection_send (conn, reply, NULL)) { 
			syslog(LOG_ERR, "Can't send reply message!") ;
		}
		dbus_message_unref (reply);
	}
	
	return ret;
}

static DBusMessage* handle_periodic_inq_req(DBusMessage *msg, void *data)
{
	write_inquiry_mode_cp inq_mode;
	periodic_inquiry_cp inq_param;
	DBusMessageIter iter;
	DBusMessage *reply = NULL;
	struct hci_dbus_data *dbus_data = data;
	uint8_t length;
	uint8_t max_period;
	uint8_t min_period;
	int sock = -1;
	int dev_id = -1;

	if (dbus_data->id == DEFAULT_DEVICE_PATH_ID) {
		if ((dev_id = hci_get_route(NULL)) < 0) {
			syslog(LOG_ERR, "Bluetooth device is not available");
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
			goto failed;
		
		}
	} else
		dev_id =  dbus_data->id;

	if ((sock = hci_open_dev(dev_id)) < 0) {
		syslog(LOG_ERR, "HCI device open failed");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_get_basic(&iter, &length);
	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &min_period);
	dbus_message_iter_next(&iter);	
	dbus_message_iter_get_basic(&iter, &max_period);

	if ((length >= min_period) || (min_period >= max_period)) {
		  reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_WRONG_PARAM);
		  goto failed;
	}

	inq_param.num_rsp = 100;
	inq_param.length  = length;

	inq_param.max_period = max_period;
	inq_param.min_period = min_period;

	/* General/Unlimited Inquiry Access Code (GIAC) */
	inq_param.lap[0] = 0x33;
	inq_param.lap[1] = 0x8b;
	inq_param.lap[2] = 0x9e;

	inq_mode.mode = 1; //INQUIRY_WITH_RSSI;

	if (hci_send_cmd(sock, OGF_HOST_CTL, OCF_WRITE_INQUIRY_MODE,
			WRITE_INQUIRY_MODE_CP_SIZE, &inq_mode) < 0) {
		syslog(LOG_ERR, "Can't set inquiry mode:%s.", strerror(errno));
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	if (hci_send_cmd(sock, OGF_LINK_CTL, OCF_PERIODIC_INQUIRY,
			PERIODIC_INQUIRY_CP_SIZE, &inq_param) < 0) {
		syslog(LOG_ERR, "Can't send HCI commands:%s.", strerror(errno));
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	} else {
		uint8_t result = 0;
		/* return TRUE to indicate that operation was completed */
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE ,&result);
	}

failed:
	if (sock > 0)
		close(sock);

	return reply;
}

static DBusMessage* handle_cancel_periodic_inq_req(DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessage *reply = NULL;
	struct hci_dbus_data *dbus_data = data;
	int sock = -1;
	int dev_id = -1;

	if (dbus_data->id == DEFAULT_DEVICE_PATH_ID) {
		if ((dev_id = hci_get_route(NULL)) < 0) {
			syslog(LOG_ERR, "Bluetooth device is not available");
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
			goto failed;
		}
	} else
		dev_id = dbus_data->id;

	if ((sock = hci_open_dev(dev_id)) < 0) {
		syslog(LOG_ERR, "HCI device open failed");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	if (hci_send_cmd(sock, OGF_LINK_CTL, OCF_EXIT_PERIODIC_INQUIRY, 0 , NULL) < 0) {
		syslog(LOG_ERR, "Send hci command failed.");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
	} else {
		uint8_t result  = 0;
		/* return TRUE to indicate that operation was completed */
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE ,&result);
	}

failed:
	if (sock > 0)
		close(sock);

	return reply;
}

static DBusMessage* handle_inq_req(DBusMessage *msg, void *data)
{
	char addr[18];
	const char array_sig[] = HCI_INQ_REPLY_SIGNATURE;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessageIter  struct_iter;
	DBusMessage *reply = NULL;
	inquiry_info *info = NULL;
	struct hci_dbus_data *dbus_data = data;
	const char *paddr = addr;
	int dev_id = -1;
	int i;
	uint32_t class = 0;
	uint16_t clock_offset;
	uint16_t flags;
	int8_t length;
	int8_t num_rsp;

	if (dbus_data->id == DEFAULT_DEVICE_PATH_ID) {
		if ((dev_id = hci_get_route(NULL)) < 0) {
			syslog(LOG_ERR, "Bluetooth device is not available");
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
			goto failed;
		}
	} else
		dev_id = dbus_data->id;

	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_get_basic(&iter, &length);
	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &num_rsp);
	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &flags);

	if ((length <= 0) || (num_rsp <= 0)) {
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_WRONG_PARAM);
		goto failed;
	}

	num_rsp = hci_inquiry(dev_id, length, num_rsp, NULL, &info, flags);

	if (num_rsp < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
	} else {
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, array_sig, &array_iter);

		for (i = 0; i < num_rsp; i++) {
			ba2str(&(info+i)->bdaddr, addr);

			clock_offset = btohs((info+i)->clock_offset);
			/* only 3 bytes are used */
			memcpy(&class, (info+i)->dev_class, 3);

			dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, NULL, &struct_iter);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING , &paddr);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32 , &class);
			dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT16 , &clock_offset);
			dbus_message_iter_close_container(&array_iter, &struct_iter);
		}

		dbus_message_iter_close_container(&iter, &array_iter);
	}

failed:
	if(info)
		bt_free(info);

	return NULL;
}

static DBusMessage* handle_role_switch_req(DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessage *reply = NULL;
	char *str_bdaddr = NULL;
	struct hci_dbus_data *dbus_data = data;
	bdaddr_t bdaddr;
	uint8_t role;
	int dev_id = -1;
	int sock = -1;

	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_get_basic(&iter, &str_bdaddr);
	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &role);

	str2ba(str_bdaddr, &bdaddr);

	dev_id = hci_for_each_dev(HCI_UP, find_conn, (long) &bdaddr);

	if (dev_id < 0) {
		syslog(LOG_ERR, "Bluetooth device failed\n");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	if (dbus_data->id != DEFAULT_DEVICE_PATH_ID && dbus_data->id != dev_id) {
		syslog(LOG_ERR, "Connection not found\n");
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_CONN_NOT_FOUND);
		goto failed;
	}

	sock = hci_open_dev(dev_id);
	
	if (sock < 0) {
		syslog(LOG_ERR, "HCI device open failed\n");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	if (hci_switch_role(sock, &bdaddr, role, 10000) < 0) {
		syslog(LOG_ERR, "Switch role request failed\n");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
	} else {
		uint8_t result = 0;
		/* return TRUE to indicate that operation was completed */
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE, &result);
	}

failed:
	return reply;
}

static DBusMessage* handle_remote_name_req(DBusMessage *msg, void *data)
{
	char name[64];
	const char *pname = name;
	DBusMessageIter iter;
	DBusMessage *reply = NULL;
	struct hci_dbus_data *dbus_data = data;
	int dev_id = -1;
	int dd = -1;
	const char *str_bdaddr;
	bdaddr_t bdaddr;
	
	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_get_basic(&iter, &str_bdaddr);
	
	str2ba(str_bdaddr, &bdaddr);

	if (dbus_data->id == DEFAULT_DEVICE_PATH_ID) {
		if ((dev_id = hci_get_route(&bdaddr)) < 0) {
			syslog(LOG_ERR, "Bluetooth device is not available");
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
			goto failed;
		}
	} else {
		dev_id = dbus_data->id;
	}

	if ((dd = hci_open_dev(dev_id)) > 0) {
	
		if (hci_read_remote_name(dd, &bdaddr, sizeof(name), name, READ_REMOTE_NAME_TIMEOUT) ==0) {
			reply = dbus_message_new_method_return(msg);
			dbus_message_iter_init_append(reply, &iter);
			dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &pname);
		} else {
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		}
	} else {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
	}
	
	if (dd > 0)
		close (dd);

failed:
	return reply;
}

static DBusMessage* handle_display_conn_req(DBusMessage *msg, void *data)
{
	struct hci_conn_list_req *cl = NULL;
	struct hci_conn_info *ci = NULL;
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessageIter  struct_iter;
	char addr[18];
	const char array_sig[] = HCI_CONN_INFO_STRUCT_SIGNATURE;
	const char *paddr = addr;
	struct hci_dbus_data *dbus_data = data;
	int i;
	int dev_id = -1;
	int sk = -1;

	if (dbus_data->id == DEFAULT_DEVICE_PATH_ID) {
		if ((dev_id = hci_get_route(NULL)) < 0) {
			syslog(LOG_ERR, "Bluetooth device is not available");
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
			goto failed;
		}
	} else {
		dev_id = dbus_data->id;
	}

	sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);

	if (sk < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	if (!(cl = malloc(MAX_CONN_NUMBER * sizeof(*ci) + sizeof(*cl)))) { 
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_NO_MEM);
		goto failed;
	}

	cl->dev_id = dev_id;
	cl->conn_num = MAX_CONN_NUMBER;
	ci = cl->conn_info;

	if (ioctl(sk, HCIGETCONNLIST, (void *) cl)) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);
	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, array_sig, &array_iter);
	
	for (i = 0; i < cl->conn_num; i++, ci++) {
		ba2str(&ci->bdaddr, addr);

		dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, NULL, &struct_iter);
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT16 ,&(ci->handle));
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING ,&paddr);
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_BYTE ,&(ci->type));
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_BYTE ,&(ci->out));
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT16 ,&(ci->state));
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_UINT32 ,&(ci->link_mode));
		dbus_message_iter_close_container(&array_iter, &struct_iter);
	}
	
	dbus_message_iter_close_container(&iter, &array_iter);
failed:

	if (sk > 0)
		close (sk);

	if (cl)
		free (cl);
	
	return reply;
}



/*****************************************************************
 *  
 *  Section reserved to Manager D-Bus message handlers
 *  
 *****************************************************************/

static DBusMessage* handle_get_devices_req(DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessageIter  struct_iter;
	DBusMessage *reply = NULL;

	struct hci_dev_list_req *dl = NULL;
	struct hci_dev_req *dr      = NULL;
	struct hci_dev_info di;
	int i;
	int sock = -1;

	char aname[BLUETOOTH_DEVICE_NAME_LEN];
	char aaddr[BLUETOOTH_DEVICE_ADDR_LEN];
	char *paddr = aaddr;
	char *pname = aname;
	const char array_sig[] = HCI_DEVICE_STRUCT_SIGNATURE;

	/* Create and bind HCI socket */
	if ((sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) {
		syslog(LOG_ERR, "Can't open HCI socket: %s (%d)", strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	dl = malloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl));

	if (!dl) {
		syslog(LOG_ERR, "Can't allocate memory");
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_NO_MEM);
		goto failed;
	}

	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(sock, HCIGETDEVLIST, dl) < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	/* active bluetooth adapter found */
	reply = dbus_message_new_method_return(msg);
	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, array_sig, &array_iter);
	dr = dl->dev_req;

	for (i = 0; i < dl->dev_num; i++, dr++) {
		if (hci_test_bit(HCI_UP, &dr->dev_opt)) {
			memset(&di, 0 , sizeof(struct hci_dev_info));
			di.dev_id = dr->dev_id;

			if (!ioctl(sock, HCIGETDEVINFO, &di)) {
				strcpy(aname, di.name);
				ba2str(&di.bdaddr, aaddr);
				dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, NULL,
						&struct_iter);
				dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING ,&pname);
				dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING ,&paddr);

				dbus_message_iter_close_container(&array_iter, &struct_iter);
			}
		}
	}

	dbus_message_iter_close_container(&iter, &array_iter);

failed:
	if (dl)
		free(dl);

	if (sock > 0)
		close (sock);

	return reply;
}

static DBusMessage* handle_not_implemented_req(DBusMessage *msg, void *data) 
{
	const char *path = dbus_message_get_path(msg);
	const char *iface = dbus_message_get_interface(msg);
	const char *method = dbus_message_get_member(msg);

	syslog(LOG_INFO, "Not Implemented - path %s iface %s method %s",
							path, iface, method);

	return NULL;
}
