/*
 * Copyright (C) 2013 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "hal-log.h"
#include "hal.h"
#include "hal-msg.h"
#include "hal-ipc.h"
#include "hal-utils.h"

static const bt_callbacks_t *bt_hal_cbacks = NULL;

#define enum_prop_to_hal(prop, hal_prop, type) do { \
	static type e; \
	prop.val = &e; \
	prop.len = sizeof(e); \
	e = *((uint8_t *) (hal_prop->val)); \
} while (0)

#define enum_prop_from_hal(prop, hal_len, hal_val, enum_type) do { \
	enum_type e; \
	if (prop->len != sizeof(e)) { \
		error("invalid HAL property %u (%u vs %zu), aborting ", \
					prop->type, prop->len, sizeof(e)); \
		exit(EXIT_FAILURE); \
	} \
	memcpy(&e, prop->val, sizeof(e)); \
	*((uint8_t *) hal_val) = e; /* enums are mapped to 1 byte */ \
	*hal_len = 1; \
} while (0)

static void handle_adapter_state_changed(void *buf, uint16_t len)
{
	struct hal_ev_adapter_state_changed *ev = buf;

	DBG("state: %s", bt_state_t2str(ev->state));

	if (bt_hal_cbacks->adapter_state_changed_cb)
		bt_hal_cbacks->adapter_state_changed_cb(ev->state);
}

static void adapter_props_to_hal(bt_property_t *send_props,
					struct hal_property *prop,
					uint8_t num_props, uint16_t len)
{
	void *buf = prop;
	uint8_t i;

	for (i = 0; i < num_props; i++) {
		if (sizeof(*prop) + prop->len > len) {
			error("invalid adapter properties(%zu > %u), aborting",
					sizeof(*prop) + prop->len, len);
			exit(EXIT_FAILURE);
		}

		send_props[i].type = prop->type;

		switch (prop->type) {
		case HAL_PROP_ADAPTER_TYPE:
			enum_prop_to_hal(send_props[i], prop,
							bt_device_type_t);
			break;
		case HAL_PROP_ADAPTER_SCAN_MODE:
			enum_prop_to_hal(send_props[i], prop,
							bt_scan_mode_t);
			break;
		case HAL_PROP_ADAPTER_SERVICE_REC:
		default:
			send_props[i].len = prop->len;
			send_props[i].val = prop->val;
			break;
		}

		DBG("prop[%d]: %s", i, btproperty2str(&send_props[i]));

		len -= sizeof(*prop) + prop->len;
		buf += sizeof(*prop) + prop->len;
		prop = buf;
	}

	if (!len)
		return;

	error("invalid adapter properties (%u bytes left), aborting", len);
	exit(EXIT_FAILURE);
}

static void adapter_prop_from_hal(const bt_property_t *property, uint8_t *type,
						uint16_t *len, void *val)
{
	/* type match IPC type */
	*type = property->type;

	switch(property->type) {
	case HAL_PROP_ADAPTER_SCAN_MODE:
		enum_prop_from_hal(property, len, val, bt_scan_mode_t);
		break;
	default:
		*len = property->len;
		memcpy(val, property->val, property->len);
		break;
	}
}

static void device_props_to_hal(bt_property_t *send_props,
				struct hal_property *prop, uint8_t num_props,
				uint16_t len)
{
	void *buf = prop;
	uint8_t i;

	for (i = 0; i < num_props; i++) {
		if (sizeof(*prop) + prop->len > len) {
			error("invalid device properties (%zu > %u), aborting",
					sizeof(*prop) + prop->len, len);
			exit(EXIT_FAILURE);
		}

		send_props[i].type = prop->type;

		switch (prop->type) {
		case HAL_PROP_DEVICE_TYPE:
			enum_prop_to_hal(send_props[i], prop,
							bt_device_type_t);
			break;
		case HAL_PROP_DEVICE_SERVICE_REC:
		case HAL_PROP_DEVICE_VERSION_INFO:
		default:
			send_props[i].len = prop->len;
			send_props[i].val = prop->val;
			break;
		}

		len -= sizeof(*prop) + prop->len;
		buf += sizeof(*prop) + prop->len;
		prop = buf;

		DBG("prop[%d]: %s", i, btproperty2str(&send_props[i]));
	}

	if (!len)
		return;

	error("invalid device properties (%u bytes left), aborting", len);
	exit(EXIT_FAILURE);
}

static void handle_adapter_props_changed(void *buf, uint16_t len)
{
	struct hal_ev_adapter_props_changed *ev = buf;
	bt_property_t props[ev->num_props];

	DBG("");

	if (!bt_hal_cbacks->adapter_properties_cb)
		return;

	len -= sizeof(*ev);
	adapter_props_to_hal(props, ev->props, ev->num_props, len);

	bt_hal_cbacks->adapter_properties_cb(ev->status, ev->num_props, props);
}

static void handle_bond_state_change(void *buf, uint16_t len)
{
	struct hal_ev_bond_state_changed *ev = buf;
	bt_bdaddr_t *addr = (bt_bdaddr_t *) ev->bdaddr;

	DBG("state %u", ev->state);

	if (bt_hal_cbacks->bond_state_changed_cb)
		bt_hal_cbacks->bond_state_changed_cb(ev->status, addr,
								ev->state);
}

static void handle_pin_request(void *buf, uint16_t len)
{
	struct hal_ev_pin_request *ev = buf;
	/* Those are declared as packed, so it's safe to assign pointers */
	bt_bdaddr_t *addr = (bt_bdaddr_t *) ev->bdaddr;
	bt_bdname_t *name = (bt_bdname_t *) ev->name;

	DBG("");

	if (bt_hal_cbacks->pin_request_cb)
		bt_hal_cbacks->pin_request_cb(addr, name, ev->class_of_dev);
}

static void handle_ssp_request(void *buf, uint16_t len)
{
	struct hal_ev_ssp_request *ev = buf;
	/* Those are declared as packed, so it's safe to assign pointers */
	bt_bdaddr_t *addr = (bt_bdaddr_t *) ev->bdaddr;
	bt_bdname_t *name = (bt_bdname_t *) ev->name;

	DBG("");

	if (bt_hal_cbacks->ssp_request_cb)
		bt_hal_cbacks->ssp_request_cb(addr, name, ev->class_of_dev,
							ev->pairing_variant,
							ev->passkey);
}

void bt_thread_associate(void)
{
	if (bt_hal_cbacks->thread_evt_cb)
		bt_hal_cbacks->thread_evt_cb(ASSOCIATE_JVM);
}

void bt_thread_disassociate(void)
{
	if (bt_hal_cbacks->thread_evt_cb)
		bt_hal_cbacks->thread_evt_cb(DISASSOCIATE_JVM);
}

static bool interface_ready(void)
{
	return bt_hal_cbacks != NULL;
}

static void handle_discovery_state_changed(void *buf, uint16_t len)
{
	struct hal_ev_discovery_state_changed *ev = buf;

	DBG("");

	if (bt_hal_cbacks->discovery_state_changed_cb)
		bt_hal_cbacks->discovery_state_changed_cb(ev->state);
}

static void handle_device_found(void *buf, uint16_t len)
{
	struct hal_ev_device_found *ev = buf;
	bt_property_t props[ev->num_props];

	DBG("");

	if (!bt_hal_cbacks->device_found_cb)
		return;

	len -= sizeof(*ev);
	device_props_to_hal(props, ev->props, ev->num_props, len);

	bt_hal_cbacks->device_found_cb(ev->num_props, props);
}

static void handle_device_state_changed(void *buf, uint16_t len)
{
	struct hal_ev_remote_device_props *ev = buf;
	bt_property_t props[ev->num_props];

	DBG("");

	if (!bt_hal_cbacks->remote_device_properties_cb)
		return;

	len -= sizeof(*ev);
	device_props_to_hal(props, ev->props, ev->num_props, len);

	bt_hal_cbacks->remote_device_properties_cb(ev->status,
						(bt_bdaddr_t *)ev->bdaddr,
						ev->num_props, props);
}

static void handle_acl_state_changed(void *buf, uint16_t len)
{
	struct hal_ev_acl_state_changed *ev = buf;
	bt_bdaddr_t *addr = (bt_bdaddr_t *) ev->bdaddr;

	DBG("state %u", ev->state);

	if (bt_hal_cbacks->acl_state_changed_cb)
		bt_hal_cbacks->acl_state_changed_cb(ev->status, addr,
								ev->state);
}

static void handle_dut_mode_receive(void *buf, uint16_t len)
{
	struct hal_ev_dut_mode_receive *ev = buf;

	DBG("");

	if (len != sizeof(*ev) + ev->len) {
		error("invalid dut mode receive event (%u), aborting", len);
		exit(EXIT_FAILURE);
	}

	if (bt_hal_cbacks->dut_mode_recv_cb)
		bt_hal_cbacks->dut_mode_recv_cb(ev->opcode, ev->data, ev->len);
}

/* handlers will be called from notification thread context,
 * index in table equals to 'opcode - HAL_MINIMUM_EVENT' */
static const struct hal_ipc_handler ev_handlers[] = {
	{	/* HAL_EV_ADAPTER_STATE_CHANGED */
		.handler = handle_adapter_state_changed,
		.var_len = false,
		.data_len = sizeof(struct hal_ev_adapter_state_changed)
	},
	{	/* HAL_EV_ADAPTER_PROPS_CHANGED */
		.handler = handle_adapter_props_changed,
		.var_len = true,
		.data_len = sizeof(struct hal_ev_adapter_props_changed) +
						sizeof(struct hal_property),
	},
	{	/* HAL_EV_REMOTE_DEVICE_PROPS */
		.handler = handle_device_state_changed,
		.var_len = true,
		.data_len = sizeof(struct hal_ev_remote_device_props) +
						sizeof(struct hal_property),
	},
	{	/* HAL_EV_DEVICE_FOUND */
		.handler = handle_device_found,
		.var_len = true,
		.data_len = sizeof(struct hal_ev_device_found) +
						sizeof(struct hal_property),
	},
	{	/* HAL_EV_DISCOVERY_STATE_CHANGED */
		.handler = handle_discovery_state_changed,
		.var_len = false,
		.data_len = sizeof(struct hal_ev_discovery_state_changed),
	},
	{	/* HAL_EV_PIN_REQUEST */
		.handler = handle_pin_request,
		.var_len = false,
		.data_len = sizeof(struct hal_ev_pin_request),
	},
	{	/* HAL_EV_SSP_REQUEST */
		.handler = handle_ssp_request,
		.var_len = false,
		.data_len = sizeof(struct hal_ev_ssp_request),
	},
	{	/* HAL_EV_BOND_STATE_CHANGED */
		.handler = handle_bond_state_change,
		.var_len = false,
		.data_len = sizeof(struct hal_ev_bond_state_changed),
	},
	{	/* HAL_EV_ACL_STATE_CHANGED */
		.handler = handle_acl_state_changed,
		.var_len = false,
		.data_len = sizeof(struct hal_ev_acl_state_changed),
	},
	{	/* HAL_EV_DUT_MODE_RECEIVE */
		.handler = handle_dut_mode_receive,
		.var_len = true,
		.data_len = sizeof(struct hal_ev_dut_mode_receive),
	},
};

static int init(bt_callbacks_t *callbacks)
{
	struct hal_cmd_register_module cmd;
	int status;

	DBG("");

	if (interface_ready())
		return BT_STATUS_DONE;

	bt_hal_cbacks = callbacks;

	hal_ipc_register(HAL_SERVICE_ID_BLUETOOTH, ev_handlers,
				sizeof(ev_handlers)/sizeof(ev_handlers[0]));

	if (!hal_ipc_init()) {
		bt_hal_cbacks = NULL;
		return BT_STATUS_FAIL;
	}

	cmd.service_id = HAL_SERVICE_ID_BLUETOOTH;

	status = hal_ipc_cmd(HAL_SERVICE_ID_CORE, HAL_OP_REGISTER_MODULE,
					sizeof(cmd), &cmd, NULL, NULL, NULL);
	if (status != BT_STATUS_SUCCESS) {
		error("Failed to register 'bluetooth' service");
		goto fail;
	}

	cmd.service_id = HAL_SERVICE_ID_SOCK;

	status = hal_ipc_cmd(HAL_SERVICE_ID_CORE, HAL_OP_REGISTER_MODULE,
					sizeof(cmd), &cmd, NULL, NULL, NULL);
	if (status != BT_STATUS_SUCCESS) {
		error("Failed to register 'socket' service");
		goto fail;
	}

	return status;

fail:
	hal_ipc_cleanup();
	bt_hal_cbacks = NULL;

	hal_ipc_unregister(HAL_SERVICE_ID_BLUETOOTH);

	return status;
}

static int enable(void)
{
	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_ENABLE, 0, NULL, 0,
								NULL, NULL);
}

static int disable(void)
{
	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_DISABLE, 0, NULL, 0,
								NULL, NULL);
}

static void cleanup(void)
{
	DBG("");

	if (!interface_ready())
		return;

	hal_ipc_cleanup();

	bt_hal_cbacks = NULL;

	hal_ipc_unregister(HAL_SERVICE_ID_BLUETOOTH);
}

static int get_adapter_properties(void)
{
	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_GET_ADAPTER_PROPS,
						0, NULL, 0, NULL, NULL);
}

static int get_adapter_property(bt_property_type_t type)
{
	struct hal_cmd_get_adapter_prop cmd;

	DBG("prop: %s (%d)", bt_property_type_t2str(type), type);

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	/* type match IPC type */
	cmd.type = type;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_GET_ADAPTER_PROP,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int set_adapter_property(const bt_property_t *property)
{
	char buf[sizeof(struct hal_cmd_set_adapter_prop) + property->len];
	struct hal_cmd_set_adapter_prop *cmd = (void *) buf;

	DBG("prop: %s", btproperty2str(property));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	adapter_prop_from_hal(property, &cmd->type, &cmd->len, cmd->val);

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_SET_ADAPTER_PROP,
				sizeof(*cmd) + cmd->len, cmd, 0, NULL, NULL);
}

static int get_remote_device_properties(bt_bdaddr_t *remote_addr)
{
	struct hal_cmd_get_remote_device_props cmd;

	DBG("bdaddr: %s", bdaddr2str(remote_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, remote_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
					HAL_OP_GET_REMOTE_DEVICE_PROPS,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int get_remote_device_property(bt_bdaddr_t *remote_addr,
						bt_property_type_t type)
{
	struct hal_cmd_get_remote_device_prop cmd;

	DBG("bdaddr: %s prop: %s", bdaddr2str(remote_addr),
						bt_property_type_t2str(type));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, remote_addr, sizeof(cmd.bdaddr));

	/* type match IPC type */
	cmd.type = type;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
					HAL_OP_GET_REMOTE_DEVICE_PROP,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int set_remote_device_property(bt_bdaddr_t *remote_addr,
						const bt_property_t *property)
{
	struct hal_cmd_set_remote_device_prop *cmd;
	uint8_t buf[sizeof(*cmd) + property->len];

	DBG("bdaddr: %s prop: %s", bdaddr2str(remote_addr),
				bt_property_type_t2str(property->type));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	cmd = (void *) buf;

	memcpy(cmd->bdaddr, remote_addr, sizeof(cmd->bdaddr));

	/* type match IPC type */
	cmd->type = property->type;
	cmd->len = property->len;
	memcpy(cmd->val, property->val, property->len);

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
					HAL_OP_SET_REMOTE_DEVICE_PROP,
					sizeof(buf), cmd, 0, NULL, NULL);
}

static int get_remote_service_record(bt_bdaddr_t *remote_addr, bt_uuid_t *uuid)
{
	struct hal_cmd_get_remote_service_rec cmd;

	DBG("bdaddr: %s", bdaddr2str(remote_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, remote_addr, sizeof(cmd.bdaddr));
	memcpy(cmd.uuid, uuid, sizeof(cmd.uuid));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
					HAL_OP_GET_REMOTE_SERVICE_REC,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int get_remote_services(bt_bdaddr_t *remote_addr)
{
	struct hal_cmd_get_remote_services cmd;

	DBG("bdaddr: %s", bdaddr2str(remote_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, remote_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
			HAL_OP_GET_REMOTE_SERVICES, sizeof(cmd), &cmd, 0,
			NULL, NULL);
}

static int start_discovery(void)
{
	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
				HAL_OP_START_DISCOVERY, 0, NULL, 0,
				NULL, NULL);
}

static int cancel_discovery(void)
{
	DBG("");

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH,
				HAL_OP_CANCEL_DISCOVERY, 0, NULL, 0,
				NULL, NULL);
}

static int create_bond(const bt_bdaddr_t *bd_addr)
{
	struct hal_cmd_create_bond cmd;

	DBG("bdaddr: %s", bdaddr2str(bd_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_CREATE_BOND,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int cancel_bond(const bt_bdaddr_t *bd_addr)
{
	struct hal_cmd_cancel_bond cmd;

	DBG("bdaddr: %s", bdaddr2str(bd_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_CANCEL_BOND,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int remove_bond(const bt_bdaddr_t *bd_addr)
{
	struct hal_cmd_remove_bond cmd;

	DBG("bdaddr: %s", bdaddr2str(bd_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_REMOVE_BOND,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int pin_reply(const bt_bdaddr_t *bd_addr, uint8_t accept,
				uint8_t pin_len, bt_pin_code_t *pin_code)
{
	struct hal_cmd_pin_reply cmd;

	DBG("bdaddr: %s", bdaddr2str(bd_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));
	cmd.accept = accept;
	cmd.pin_len = pin_len;
	memcpy(cmd.pin_code, pin_code, sizeof(cmd.pin_code));

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_PIN_REPLY,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int ssp_reply(const bt_bdaddr_t *bd_addr, bt_ssp_variant_t variant,
					uint8_t accept, uint32_t passkey)
{
	struct hal_cmd_ssp_reply cmd;

	DBG("bdaddr: %s", bdaddr2str(bd_addr));

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	memcpy(cmd.bdaddr, bd_addr, sizeof(cmd.bdaddr));
	/* type match IPC type */
	cmd.ssp_variant = variant;
	cmd.accept = accept;
	cmd.passkey = passkey;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_SSP_REPLY,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static const void *get_profile_interface(const char *profile_id)
{
	DBG("%s", profile_id);

	if (!interface_ready())
		return NULL;

	if (!strcmp(profile_id, BT_PROFILE_SOCKETS_ID))
		return bt_get_sock_interface();

	if (!strcmp(profile_id, BT_PROFILE_HIDHOST_ID))
		return bt_get_hidhost_interface();

	if (!strcmp(profile_id, BT_PROFILE_PAN_ID))
		return bt_get_pan_interface();

	if (!strcmp(profile_id, BT_PROFILE_ADVANCED_AUDIO_ID))
		return bt_get_a2dp_interface();

	return NULL;
}

static int dut_mode_configure(uint8_t enable)
{
	struct hal_cmd_dut_mode_conf cmd;

	DBG("enable %u", enable);

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	cmd.enable = enable;

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_DUT_MODE_CONF,
					sizeof(cmd), &cmd, 0, NULL, NULL);
}

static int dut_mode_send(uint16_t opcode, uint8_t *buf, uint8_t len)
{
	uint8_t cmd_buf[sizeof(struct hal_cmd_dut_mode_send) + len];
	struct hal_cmd_dut_mode_send *cmd = (void *) cmd_buf;

	DBG("opcode %u len %u", opcode, len);

	if (!interface_ready())
		return BT_STATUS_NOT_READY;

	cmd->opcode = opcode;
	cmd->len = len;
	memcpy(cmd->data, buf, cmd->len);

	return hal_ipc_cmd(HAL_SERVICE_ID_BLUETOOTH, HAL_OP_DUT_MODE_SEND,
					sizeof(cmd_buf), cmd, 0, NULL, NULL);
}

static const bt_interface_t bluetooth_if = {
	.size = sizeof(bt_interface_t),
	.init = init,
	.enable = enable,
	.disable = disable,
	.cleanup = cleanup,
	.get_adapter_properties = get_adapter_properties,
	.get_adapter_property = get_adapter_property,
	.set_adapter_property = set_adapter_property,
	.get_remote_device_properties = get_remote_device_properties,
	.get_remote_device_property = get_remote_device_property,
	.set_remote_device_property = set_remote_device_property,
	.get_remote_service_record = get_remote_service_record,
	.get_remote_services = get_remote_services,
	.start_discovery = start_discovery,
	.cancel_discovery = cancel_discovery,
	.create_bond = create_bond,
	.remove_bond = remove_bond,
	.cancel_bond = cancel_bond,
	.pin_reply = pin_reply,
	.ssp_reply = ssp_reply,
	.get_profile_interface = get_profile_interface,
	.dut_mode_configure = dut_mode_configure,
	.dut_mode_send = dut_mode_send
};

static const bt_interface_t *get_bluetooth_interface(void)
{
	DBG("");

	return &bluetooth_if;
}

static int close_bluetooth(struct hw_device_t *device)
{
	DBG("");

	cleanup();

	return 0;
}

static int open_bluetooth(const struct hw_module_t *module, char const *name,
					struct hw_device_t **device)
{
	bluetooth_device_t *dev = malloc(sizeof(bluetooth_device_t));

	DBG("");

	memset(dev, 0, sizeof(bluetooth_device_t));
	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *) module;
	dev->common.close = close_bluetooth;
	dev->get_bluetooth_interface = get_bluetooth_interface;

	*device = (struct hw_device_t *) dev;

	return 0;
}

static struct hw_module_methods_t bluetooth_module_methods = {
	.open = open_bluetooth,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 1,
	.version_minor = 0,
	.id = BT_HARDWARE_MODULE_ID,
	.name = "BlueZ Bluetooth stack",
	.author = "Intel Corporation",
	.methods = &bluetooth_module_methods
};
