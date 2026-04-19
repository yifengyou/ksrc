.. SPDX-License-Identifier: GPL-2.0

=====================
Hisilicon UBUS Driver
=====================

Hisilicon UBUS Driver (abbreviated as Hisi UBUS) is a UnifiedBus (UB)
specification management subsystem specifically implemented for Hisi chips. It
provides a subsystem operation interfaces implementation::

	static const struct ub_manage_subsystem_ops hisi_ub_manage_subsystem_ops = {
		.vendor = HISI_VENDOR_ID,
		.controller_probe = ub_bus_controller_probe,
		.controller_remove = ub_bus_controller_remove,
		.ras_handler_probe = ub_ras_handler_probe,
		.ras_handler_remove = ub_ras_handler_remove
	};

including probe/remove methods for the UB bus controller and ub ras handler.
Each specification management subsystem has a unique vendor id to identify the
provider. This vendor id is set to the vendor field of
``ub_manage_subsystem_ops`` implementation. During UB bus controller probe, a
ub_bus_controller_ops will be set to the UB bus controller, message device and
debug file system will be initialized. During UB bus controller remove, ops
will be unset, message device will be removed and debug file system will be
uninitialized.

During module init, hisi_ub_manage_subsystem_ops is registered to Ubus driver
via the ``register_ub_manage_subsystem_ops()`` method provided by Ubus driver::

	int register_ub_manage_subsystem_ops(const struct ub_manage_subsystem_ops *ops)

When module is being unloaded, Ubus driver's
``unregister_ub_manage_subsystem_ops()`` is called to unregister the subsystem
operation interfaces::

	void unregister_ub_manage_subsystem_ops(const struct ub_manage_subsystem_ops *ops)

Hisi UBUS Controller Driver
===========================
Hisi UBUS provides a ub bus controller operation interfaces implementation::

	static struct ub_bus_controller_ops hi_ubc_ops = {
		.eu_table_init = hi_eu_table_init,
		.eu_table_uninit = hi_eu_table_uninit,
		.eu_cfg = hi_eu_cfg,
		.mem_decoder_create = hi_mem_decoder_create,
		.mem_decoder_remove = hi_mem_decoder_remove,
		.register_ubmem_irq = hi_register_ubmem_irq,
		.unregister_ubmem_irq = hi_unregister_ubmem_irq,
		.register_decoder_base_addr = hi_register_decoder_base_addr,
		.entity_enable = hi_send_entity_enable_msg,
	};

including init/uninit method for EID/UPI table, create/remove method for UB
memory decoder, register/unregister method for UB memory decoder interrupts
and so on.

UB Message Core Driver
======================
Hisi UBUS implements a message device that provides a set of operations::

	static struct message_ops hi_message_ops = {
		.sync_request = hi_message_sync_request,
		.response = hi_message_response,
		.sync_enum = hi_message_sync_enum,
		.vdm_rx_handler = hi_vdm_rx_msg_handler,
		.send = hi_message_send,
	};

including synchronous message sending, synchronous enumeration message
sending, response message sending, vendor-defined message reception handling
and so on. After device creation, ``message_device_register()`` method of Ubus
driver is called to register the device to the Ubus driver message framework::

	int message_device_register(struct message_device *mdev)

This framework provides a unified interface for message transmission and
reception externally.

Hisi UBUS Local Ras Error Handler
=================================
Hisi UBUS provides a local RAS handling module to detect and process errors
reported on the UB bus. It offers error printing and registry dump, determines
whether recovery is needed based on error type and severity, and can reset
ports for port issues in cluster environment.

UB Vendor-Defined Messages Manager
==================================
Hisi UBUS defines several vendor-defined messages, implements messages'
transmission and processing. These private messages are mainly used for
managing the registration, release, and state control of physical and
virtual devices.