.. SPDX-License-Identifier: GPL-2.0

======================================
How To Write Linux UB Device Drivers
======================================

UnifiedBus (abbreviated as UB) is an interconnection technology and
architecture designed for computing systems. It unifies the communication
between IO, memory access, and various processing units within the same
interconnection technology framework, enabling high-performance data transfer,
unified resource management, efficient collaboration, and effective programming
in computing systems. Resource management is one of its key features,
implemented through a combination of software and hardware. The UB Bus Driver
(referred to as the UBUS Driver) implements the software portion of this
feature. This document provides a brief overview of the components within the
UBUS Driver framework and how to develop UB device drivers within this driver
framework. See more on the UB spec <https://www.unifiedbus.com/en>.

Composition of the UBUS Driver
==============================
The UBUS Driver consists of two parts. The first part is the common
implementation section, which will be developed according to the UB
specification requirements. The second part is the proprietary implementation by
each manufacturer, which is based on the specific circuit designs of each host
manufacturer. Each host manufacturer can provide differentiated functionalities
in this part of the code.

If the UBUS subsystem is not configured (CONFIG_UB_UBUS is not set), most of
the UBUS functions described below are defined as inline functions either
completely empty or just returning an appropriate error codes to avoid
lots of ifdefs in the drivers.

The figure below illustrates the internal composition and system boundaries of
the UBUS Driver.

.. code-block:: none

 +----------------------------------------------------------+
 |                     ub device driver                     |
 +----------------------------------------------------------+
				^
				|
				v
 +----------------------------------------------------------+
 |                        ubus driver                       |
 |                                                          |
 |   +--------------------------------------------------+   |
 |   |           ubus driver vendor-specific            |   |
 |   +--------------------------------------------------+   |
 |                                                          |
 |   +--------------------------------------------------+   |
 |   |                ubus driver common                |   |
 |   |                                                  |   |
 |   |  +------+ +--------+ +------+ +-------+ +-----+  |   |     +---------+
 |   |  | enum | | config | | port | | route | | msg |  |   | <-> | GIC/ITS |
 |   |  +------+ +--------+ +------+ +-------+ +-----+  |   |     +---------+
 |   |      +------------+ +--------+ +-----------+     |   |
 |   |      | controller | | entity | | interrupt |     |   |     +------------+
 |   |      +------------+ +--------+ +-----------+     |   | <-> | IOMMU/UMMU |
 |   |  +---------+ +----------+ +------+ +----------+  |   |     +------------+
 |   |  | decoder | | resource | | pool | | instance |  |   |
 |   |  +---------+ +----------+ +------+ +----------+  |   |
 |   | +-----+ +------+ +-------+ +---------+ +-------+ |   |
 |   | | ras | | link | | reset | | hotplug | | sysfs | |   |
 |   | +-----+ +------+ +-------+ +---------+ +-------+ |   |
 |   |           +------+ +---------------+             |   |
 |   |           | ubfi | | bus framework |             |   |
 |   |           +------+ +---------------+             |   |
 |   +--------------------------------------------------+   |
 +----------------------------------------------------------+
			      ^
			      |
			      v
 +----------------------------------------------------------+
 |                    hardware/firmware                     |
 +----------------------------------------------------------+

The following briefly describes the functions of each submodule within the
UBUS driver:

  - enum: implement network topology scanning and device enumeration
          functionality
  - config: enable access to the device configuration space
  - port: manage device ports
  - route: implement the configuration of the routing table
  - msg: implement message assembly and transmission/reception processing
         for management messages
  - controller: initialization and de-initialization of the UB controller
  - entity: enable device configuration, multi-entity management, and other
            functionalities
  - interrupt: implement USI interrupt functionality
  - decoder: implement address decoding functionality for MMIO access to
             device resource space
  - resource: manage the MMIO address space allocated by the user host to
              the device
  - pool: implementation of pooled message processing
  - instance: implement bus instance management
  - ras: implement handling for RAS exceptions
  - link: implement processing of link messages
  - reset: implement the reset function
  - hotplug: enable hot-plug functionality for the device
  - sysfs: implement sysfs attribute files
  - ubfi: implement parsing of the UBRT table
  - bus framework: implementation of the Ubus Driver Framework

Structure of UB device driver
=============================
In Linux, the ``ub_driver`` structure is used to describe a UB device driver.
The `struct ub_driver` is employed to represent a UB device driver, and
the structure definition is as follows.

.. kernel-doc:: include/ub/ubus/ubus.h
   :functions: ub_driver

This structure includes a matchable device table (`id_table`), a probe function,
a remove function, a shutdown function, error handling, and other functionalities.
The following content will provide a reference for the implementation of these
features.

Rules for Device and Driver Matching
------------------------------------
The matching rules for UnifiedBus devices and drivers are relatively flexible,
allowing for any combination of the following five matching entries in the
`struct ub_device_id` within the device driver to achieve the target matching rule:

  - GUID's Vendor ID
  - GUID's Device ID
  - Configuration Space Module Vendor ID
  - Configuration Space Module ID
  - Configuration Space Class Code

The ID table is an array of ``struct ub_device_id`` entries ending with an
all-zero entry. Definitions with static const are generally preferred.

.. kernel-doc:: include/linux/mod_devicetable.h
   :functions: ub_device_id

Most drivers only need ``UB_ENTITY()`` or ``UB_ENTITY_MODULE`` or
``UB_ENTITY_CLASS()`` to set up a ub_device_id table.

The following is an example::

	static const struct ub_device_id sample_tbl[] = {
		{ 0xCC08, 0xA001, UB_ANY_ID, UB_ANY_ID, 0, 0 },
		{ UB_ENTITY(0xCC08, 0xA001), 0, 0 },
		{ UB_ENTITY_MODULE(0xCC08, 0xA001, 0xCC08, 0xA001), 0, 0 },
		{ UB_ENTITY_CLASS(0x0200, 0xffff) },
	};

New UB IDs may be added to a device driver ub_ids table at runtime
as shown below::

  echo "vendor device modulevendor moduleid class class_mask driver_data" > \
  /sys/bus/ub/drivers/sample/new_id

All fields are passed in as hexadecimal values (no leading 0x).
The vendor and device fields are mandatory, the others are optional. Users
need pass only as many optional fields as necessary:

  - modulevendor and moduledevice fields default to UB_ANY_ID (FFFFFFFF)
  - class and classmask fields default to 0
  - driver_data defaults to 0UL.
  - override_only field defaults to 0.

Note that driver_data must match the value used by any of the ub_device_id
entries defined in the driver. This makes the driver_data field mandatory
if all the ub_device_id entries have a non-zero driver_data value.

Once added, the driver probe routine will be invoked for any unclaimed
UB devices listed in its (newly updated) ub_ids list.

Register UB Device Driver
-------------------------
The UB device driver uses `ub_register_driver` to register the device driver.
During the registration process, the matching between the device and the
driver will be triggered, with the matching rules referenced in the previous
section.

UB Device Driver Probe Process Reference
----------------------------------------
- Call `ub_set_user_info` to configure the user host information into the entity
	Each entity's configuration space has corresponding user register
	information, such as user EID, token ID, etc. Before the device driver
	starts using the device, it needs to configure the user host information
	for the device.

- Call `ub_entity_enable` to configure the access path between the host and the device
	Before using the device, you need to enable the bidirectional channel
	switch for accessing the device from the user host and vice versa.
	This is achieved by configuring the device configuration space registers.

- Set the DMA mask size
	The device driver can reconfigure this field segment based on the
	device's DMA addressing capability. The default configuration is 32-bit.

- Call the kernel DMA interface to request DMA memory
	The device driver requests DMA memory through the DMA interface provided
	by the kernel to prepare for subsequent device DMA operations.

- Call `ub_iomap` to complete the MMIO access mapping for the resource space
	The device resource space stores private configurations related to device
	driver capabilities. Before accessing the device resource space, you need
	to call the ioremap interface to complete address mapping. The ub_iomap
	interface uses the device attribute, while the ub_iomap_wc interface
	uses the writecombine attribute.

- Call `ub_alloc_irq_vectors` or `ub_alloc_irq_vectors_affinity` to complete
	the interrupt request, and then call the kernel's interrupt registration API.

- Initiate specific business functions

UB Device Driver Removal Process Reference
------------------------------------------
- Stop specific business functions
- Invoke the kernel's interrupt unregistration API, call ub_disable_intr, to
  complete the unregistration of the interrupt handler and release the interrupt
- Call ub_iounmap to demap the MMIO access space
- Invoke the kernel's DMA interface to release DMA memory
- Call ub_entity_enable to close the access path between the host and the device
- Call ub_unset_user_info to clear the user host information configured to the
  entity

UB Device Driver Shutdown
-------------------------
The UB device shutdown is triggered during the system shutdown or restart
process, and the UB device driver needs to stop the service flow in the shutdown
interface.

UB Device Driver Virtual configure
----------------------------------

If the MUE supports multiple UEs, the device driver needs to provide
`virt_configure` callback. the UEs can be enabled or disabled to facilitate
direct connection to virtual machines for use. The bus driver will cyclically
call the virt_configure callback of the device driver to enable and disable
each UE in sequence. Within the virt_configure function of the device driver,
it needs to call `ub_enable_ue` and `ub_disable_ue` provided by the bus driver
to create and destroy UEs, at the same time, private processing logic can
also be executed.

UE can be enabled and disabled through sysfs. The process is as follows::

  1. Check the number of UEs currently supported by the MUE
    # cat /sys/bus/ub/devices/.../ub_totalues
  2. Specify the number of enabled UEs within the maximum UE quantity range
    # echo 3 > /sys/bus/ub/devices/.../ub_numues
  3. Disable UEs
    # echo 0 > /sys/bus/ub/devices/.../ub_numues

UB Device Driver Virtual notify
-------------------------------

If the device supports multiple UEs and the MUE device driver wants to be
aware of UE state changes, `virt_notify` hook function can be implemented to
capture the UE state.

UB Device Driver Activate and Deactivate
----------------------------------------

The bus driver supports maintaining the working status of entities, indicating
whether an entity is in operation. It also provides corresponding interfaces
for controlling devices to enter or exit the working state, such as
`ub_activate_entity` and `ub_deactivate_entity`. If the device driver needs
to perform any special procedures, it must implement the corresponding activate
and deactivate hook functions.

UB Device Driver RAS handler
----------------------------

The bus driver provides a set of hooks for RAS processing, creating an
opportunity window to notify device drivers when handling events such as
resets and RAS, allowing them to execute corresponding processing measures.
Currently implemented hooks include `reset_prepare`, `reset_done`,
`error_detected`, and `resource_enabled`. Device drivers can optionally
provide corresponding implementations to execute their own private processing.

Uninstall UB Device Driver
--------------------------
The UB device driver uses `ub_unregister_driver` to unregister the driver. This
interface call will perform a remove operation on all devices matched by the
driver, ultimately removing the UB device driver from the system.

How to find UB devices manually
===============================

UBUS provides several interfaces to obtain ub_entities. You can search for them
using keywords such as GUID, EID, or entity number. Or you can find an entire
class of devices using vendor ID and device ID.

How to access UB Configuration space
====================================

You can use `ub_config_(read|write)_(byte|word|dword)` to access the config
space of an entity represented by `struct ub_entity *`. All these functions return
0 when successful or an error code. Most drivers expect that accesses to valid UB
entities don't fail.

The macros for configuration space registers are defined in the header file
include/uapi/ub/ubus/ubus_regs.h.

Vendor and device identifications
=================================

Do not add new device or vendor IDs to include/ub/ubus/ubus_ids.h unless they
are shared across multiple drivers.  You can add private definitions in
your driver if they're helpful, or just use plain hex constants.

The device IDs are arbitrary hex numbers (vendor controlled) and normally used
only in a single location, the ub_device_id table.

Please DO submit new vendor/device IDs to <ub-spec@lists.unifiedbus.com>.
There's a mirror of the ub.ids file at https://gitee.com/openeuler/ubutils/ub.ids.
