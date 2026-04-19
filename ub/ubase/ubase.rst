.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.

==============================
UNIFIEDBUS BASE DRIVER (UBASE)
==============================

UB is a new interconnection protocol and architecture designed for computing
systems, see more on the UB spec <https://www.unifiedbus.com/en>.

This document will introduce the composition of the UBASE Driver and how to
write a UB auxiliary device driver under the Auxiliary Bus framework of UBASE,
also include related debugging information.

Overview
========

UBASE driver is one of the base driver for UB network, based on UB hardware
interface, providing public resource management and abstraction of common
interfaces for the upper layer UB device drivers such as unic, udma, cdma, fwctl,
pmu and uvb, which are auxiliary devcie drivers. UBASE driver also offers
device-driver matching interfaces for the upper-layer drivers based on auxiliary
bus, isolating different auxiliary device drivers, like udma driver using urma
core and unic driver with TCP/IP stack. And ubase has the ability of extending
auxiliary device list to transfer the UB hardware for further use and richer
features.

UBASE includes the functionalities of ubus device management, resource management,
auxiliary device management, entity management, query the specific capabilities
of the device and so on. It's also the base of other auxiliary device drivers
which must be loaded before them.

.. code-block:: none

     +----------+ +----------+ +----------+ +---------+ +-----------+ +---------+
     |   unic   | |   udma   | |   cdma   | |   pmu   | |   ubctl   | |   uvb   |
     +----------+ +----------+ +----------+ +---------+ +-----------+ +---------+
           ^            ^            ^           ^            ^            ^
           |            |            |           |            |            |
           v            v            v           v            v            v
     +--------------------------------------------------------------------------+
     |                               auxiliary bus                              |
     +--------------------------------------------------------------------------+
                                           ^
                                           |
                                           v
     +--------------------------------------------------------------------------+
     |                                   ubase                                  |
     |  +-----+ +------+ +---------+ +-------+ +----+ +-----+ +-----+ +------+  |
     |  | dev | | main | | debugfs | | ctrlq | | eq | | arq | | ras | | ubus |  |
     |  +-----+ +------+ +---------+ +-------+ +----+ +-----+ +-----+ +------+  |
     |  +-----+ +----+ +---------+ +------+ +-----+ +-------+ +----+ +-------+  |
     |  | cmd | | hw | | mailbox | | pmem | | qos | | reset | | tp | | stats |  |
     |  +-----+ +----+ +---------+ +------+ +-----+ +-------+ +----+ +-------+  |
     +--------------------------------------------------------------------------+
                        ^                        ^                     ^
                        |                        |                     |
                        |                        v                     v
                        |              +------------------+  +------------------+
                        |              |       ubus       |  |       ummu       |
                        |              +------------------+  +------------------+
                        |                        ^                     ^
                        |                        |                     |
                        v                        v                     v
     +--------------------------------------------------------------------------+
     |                                 firmware                                 |
     +--------------------------------------------------------------------------+

Below is the summary for the submodules in ubase driver:

  - 1) main: implement module_init/exit().
  - 2) dev: implement auxiliary bus init/uninit function, resource creating and
       auxiliary device enable/disable.
  - 3) cmd: implement 'command queue' to interact with firmware.
  - 4) ctrlq: implement 'control queue' to interact with management software.
  - 5) mailbox: implement 'mailbox' configuration to interact with hardware
       through `cmdq`.
  - 6) hw: implement interaction with firmware and hardware for functions.
  - 7) reset: implement hardware reset handling for ubase driver.
  - 8) tp: implement tp layer context BA and context creation.
  - 9) debugfs: implement kernel debugfs to obtain debugging information.
  - 10) qos: implement quality of service for upper communication modules.
  - 11) ras: implement hardware error handler.
  - 12) ubus: implement interaction with module `ubus`.
  - 13) eq: event queue including asynchronous and completion event.

Supported Hardware
==================

UBUS vendor/device pairs:

========= =========== ======================================
Vendor ID  Device ID  Description
========= =========== ======================================
0xCC08    0xA001      Kunpeng URMA MUE (Management UB Entity)
0xCC08    0xA002      Kunpeng URMA UE (UB Entity)
0xCC08    0xA003      Kunpeng CDMA MUE (Management UB Entity)
0xCC08    0xA004      Kunpeng CDMA UE (UB Entity)
0xCC08    0xA005      Kunpeng PMU MUE (Management UB Entity)
0xCC08    0xA006      Kunpeng PMU UE (UB Entity)
0xCC08    0xD802      Ascend URMA MUE (Management UB Entity)
0xCC08    0xD803      Ascend URMA UE (UB Entity)
0xCC08    0xD804      Ascend CDMA MUE (Management UB Entity)
0xCC08    0xD805      Ascend CDMA UE (UB Entity)
0xCC08    0xD806      Ascend PMU MUE (Management UB Entity)
0xCC08    0xD807      Ascend PMU UE (UB Entity)
0xCC08    0xD80B      Ascend UBOE MUE (Management UB Entity)
0xCC08    0xD80C      Ascend UBOE UE (UB Entity)
========= =========== ======================================

Supported Auxiliary device
==========================

UB Auxiliary bus device/driver pairs:

=========    ====   ====   ====   =====   ===    ===
Device ID    unic   udma   cdma   fwctl   pmu    uvb
=========    ====   ====   ====   =====   ===    ===
0xA001        O      O      X       X      X      O
0xA002        X      O      X       X      X      O
0xA003        X      X      O       X      X      X
0xA004        X      X      O       X      X      X
0xA005        X      X      X       O      O      X
0xA006        X      X      X       O      O      X
0xD802        O      O      X       X      X      O
0xD803        X      O      X       X      X      O
0xD804        X      X      O       X      X      X
0xD805        X      X      O       X      X      X
0xD806        X      X      X       O      O      X
0xD807        X      X      X       O      O      X
0xD80B        O      O      X       X      X      O
0xD80C        X      O      X       X      X      O
=========    ====   ====   ====   =====   ===    ===

If anyone wants to support a new auxiliary device driver based on ubase, after
adding an specific device id matched with vendor id, extending the driver
list is necessary as follows::
	enum ubase_drv_type {
		UBASE_DRV_UNIC,
		UBASE_DRV_UDMA,
		UBASE_DRV_CDMA,
		UBASE_DRV_FWCTL,
		UBASE_DRV_PMU,
		UBASE_DRV_UVB,
		UBASE_DRV_MAX,
	};

Next, `struct ubase_adev_device` is supposed to be extended by the new device
driver with its name filled in `suffix` and supported capabilities function
hooking up to the handling named `is_supported`. Following is an example driver
`unic` in ``ubase_dev.c``::
	static struct ubase_adev_device {
		const char *suffix;
		bool (*is_supported)(struct ubase_dev *dev);
	} ubase_adev_devices[UBASE_DRV_MAX] = {
		[UBASE_DRV_UNIC] = {
			.suffix = "unic",
			.is_supported = &ubase_dev_unic_supported
		},
	};

Then the new driver can fulfill `struct auxiliary_driver` ops allowing auxiliary
bus transfer handling `probe` to initialize the new driver and handling `remove`
to uninitialize it.

Module parameters
=================
UBASE driver includes one module parameter for now as `debug`. The default
parameter can support full function of ubase and related drivers, but in some
special scene like locating problems, debug information is necessary.

debug

This parameter controls the print level of ubase driver, preventing printing
debug information like `UBUS ELR start` to locate the position of driver running,
which may be helpful when doing problem identification to clarify the line of
code where the problem occurs.

This parameter is not supposed to set in loading driver but changed in a system
configuration file created by ubase, which set to be `0` means disable in default
as not showing all debug information. If the user wants to enable the debug printing,
the file `/sys/module/ubase/parameters/debug` can be set to an integer value
`except 0` as enable like `1` through the command `echo`, following line shows::
	echo 1 > /sys/module/ubase/parameters/debug

Or set the insmod parameter `debug` to value `1`, like the following:

.. code-block:: none

	insmod ubase.ko debug=1

Debugging
=========
UBASE driver supports to obtain debug related information for users through
`debug filesystem` set by Linux kernel, which helps a lot for problem locating
and quick overview of ubase driver. The ubase debugfs includes
`context querying in software and hardware`, `reset information`,
`capabilities information`, `activate record`, `qos information`,
`prealloc memory information`.

Through debugfs interfaces when `CONFIG_DEBUG_FS` is enabled, the users can obtain
these information from the directory in system::
	/sys/kernel/debug/ubase/<entity_num>/

1) context querying

  UBASE driver supports to query the context created in ubase for all auxiliary
  device drivers, including aeq, ceq, tp and tpg context stored in both software
  and hardware to verify whether configuration satisfy using demand. Note, the
  context ended with `hw` means hardware, like `aeq_context_hw`, and another one
  without `hw` means that stored in software, such as `aeq_context`.

2) reset information

  UBASE driver supports to query all kinds of reset implementation counts,
  including ELR reset, port reset, himac reset, total finish count in software
  and hardware, and failed count.

3) capabilities information

  UBASE driver supports to query the capabilities information in self device,
  which can be used for upper-layer drivers, and also resource size for creating.

4) activate record

  UBASE driver supports to query activate record about the hardware, including
  activate and deactivate counts and the exact time of these actions.

5) qos information

  UBASE driver supports to query quality of service(qos) information configured
  in hardware, about configuration set by ets, tm and `management software`,
  including `tc &tc group`, `rqmt table`, `mapping in vl, sl and dscp`,
  `tm port`, `tm priority`, `qset and queue information` and so on.

6) prealloc memory information

  UBASE driver supports to query the hulk pages allocated by ubase for both
  common use and udma use.

Functionality dependencies
==========================
Some functions in ubase driver rely on configuration of `management software` as
the manager, the following shows dependencies of ubase:
  - 1) Qos configuration: `management software` take the responsibility of
       `entity creation and distribute TQMS queue` and `mapping from sl to vl`.
  - 2) TP context: `management software` take the response of creating TP layer
       context for common use, including tp context basic address (BA),
       tp group(tpg) context BA, TP extdb buff, TP timer buff, CC context BA,
       Dest address BA, Seid_upi BA, TPM BA and so on.
  - 3) Reset: The reset process needs collaborative cooperation between ubase
       driver and `management software`, including stop flow, resume flow,
       reconstruct TP layer context and so on.

Support
=======
If there is any issue or question, please email the specific information related
to the issue or question to <dev@openeuler.org> or vendor's support channel.