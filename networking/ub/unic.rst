.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.

===========
UNIC Driver
===========

Overview
========
unic is a UB (UnifiedBus) networking driver based on ubase driver's auxiliary
device through auxiliary bus, supporting both ethernet and UB link layer.
See :ref:`Documentation/ub/ubase/ubase.rst` for more information about ubase
driver and :ref:`Documentation/networking/ub/ubl.rst` for more information about
UB link layer.

.. code-block:: none

      +---------------------------------------------------------------+
      |                     networking stack                          |
      +---------------------------------------------------------------+
               ^                    ^                         ^
               |                    |                         |
               |                    |                         |
               v                    |                         v
      +----------------+            |                 +---------------+
      | Ethernet Layer |            |                 | UB Link Layer |
      +----------------+            |                 +---------------+
               ^                    |                         ^
               |                    |                         |
               |                    |                         |
               v                    v                         v
      +---------------------------------------------------------------+
      |                                                               |
      |                             unic                              |
      |                                                               |
      | +------+ +-----+ +--------+ +---------+ +-------+ +---------+ |
      | | main | | dev | | netdev | | ethtool | | dcbnl | | tx & rx | |
      | +------+ +-----+ +--------+ +---------+ +-------+ +---------+ |
      |    +---------+ +-----------+ +-----+ +---------+ +-------+    |
      |    | channel | | comm_addr | | crq | | rack_ip | | reset |    |
      |    +---------+ +-----------+ +-----+ +---------+ +-------+.   |
      |        +----+ +--------+ +-------+ +-------+ +------+         |
      |        | hw | | qos_hw | | event | | stats | | guid |         |
      |        +----+ +--------+ +-------+ +-------+ +------+         |
      +---------------------------------------------------------------+
                    ^                                 ^
                    |                                 |
                    |                                 |
                    v                                 |
      +-------------------------------+               |
      |        auxiliary_bus          |               |
      +-------------------------------+               |
                    ^                                 |
                    |                                 |
                    |                                 |
                    v                                 v
      +---------------------------------------------------------------+
      |                             ubase                             |
      +---------------------------------------------------------------+

The main submodules in unic driver:

::

   main
     implement module_init(), module_exit() and 'struct auxiliary_driver' for
     the auxiliary device of ubase driver.

   dev
     implement init & uninit function and periodic task handling for unic's
     netdev.

   netdev
     implement 'struct net_device_ops' for unic's netdev.

   ethtool
     implement 'struct ethtool_ops' for unic's netdev.

   dcbnl
     implement 'dcbnl_rtnl_ops' for unic's netdev.

   tx & rx
     implement packet send and receive handling.

   channel
     implement channel handling for unic's netdev.

   comm_addr & rack_ip
     implement the ip address handling in UB mode.

   reset
     implement the entity reset handling.

   crq
     implement link status change handling through ctrl (Control) receive queue.

   hw & qos_hw
     implement generic hw and qos related configuration access function.

   stats
     implement hw statistics collecting funciton.

   event
     implement asynchronous event reporting interface.

   guid
     implement the GUI (Globally Unique Identifier) querying in UB mode.

Hardware Supported
==================

This driver is compatible with below UB devices:

.. code-block:: none

   +--------------+--------------+
   |  Vendor ID   |  Device ID   |
   +==============+==============+
   |   0xCC08     |    0xA001    |
   +--------------+--------------+
   |   0xCC08     |    0xD802    |
   +--------------+--------------+
   |   0xCC08     |    0xD80B    |
   +--------------+--------------+

Note 'lsub' from ubutils package can be used to tell if the above device is
available in the system, see <https://gitee.com/openeuler/ubutils>:

::

   # lsub
   <00009> UB network controller <0002>: Huawei Technologies Co., Ltd. URMA management ub entity <cc08>:<a001>

Additional Features and Configurations
======================================

UB Link
-------
UB Link is a link layer defined by UB, which has the same layer of the existing
ethernet, and firmware will report the mode of the hardware port to the driver
through hardware capability reporting, UB_Link or ETH_MAC.

In UB mode, the link layer is UB and its L2 address is the GUID as below
example:

::

   # ip -s addr
   1: ublc0d0e2: <POINTOPOINT,NOARP,UP,LOWER_UP> mtu 1500 qdisc mq state UP mode DEFAULT group default qlen 1000
       link/ub cc:08:d8:02:d2:0a:00:00:00:00:00:00:00:00:00:01 peer 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00

Note: port speed auto-negotiation is not supported in UB mode.


IP Address Configuration
------------------------
IP address configuration must be performed by the management software, after
receiving the IP address configuration through crq event, the driver will
update the IP address configuration to networking stack using the netlink API.

ELR support
-----------
ELR (Entity Level Reset) is the error recovery defined in UB, which can be
triggered by packet transmiting timeout, see unic_tx_timeout() or using the
below cmd to trigger ELR manually:

::

   # ethtool --reset <netdev> dedicated
   # echo 1 > /sys/class/net/<netdev>/device/reset

Debugging
=========

module parameters
-----------------
Enable more verbose unic driver specific debug message log by setting **debug**
to non-zero, and enable network interface debug message log by configuring
**netif_debug**, for example:

::

   # insmod unic.ko debug=1 netif_debug=0xFFFFF

Debugfs Interface
-----------------
When CONFIG_DEBUG_FS is enabed, below debug info is accessible through
/sys/kernel/debug/ubase/$entity_num/unic/:

.. code-block:: none

   ├── clear_link_status_record: clear the link status record by reading
   ├── link_status_record: show the link status record debug info
   ├── promisc_cfg_hw: show the promisc configuration in hardware
   ├── rss_cfg_hw: show the rss configuration in hardware
   ├── page_pool_info: show the rx page_pool buffer debug info
   ├── caps_info: show the capability debug info
   ├── dev_info: show the device info, such as max MTU
   ├── qos/: show the qos related debug info
   ├── vport/: show the UE (UB Entity) debug info of MUE (Management UB Entity)
   ├── context/: show object context debug info, such as JFS (Jetty For Send)
   ├── ip_tbl/: show the IP address configuration debug info

Note, the bus-info in the output of below cmd can be used to query the entity
number for a unic driver's netdev, which has an entity number of "00002" as
below example:

::

   # ethtool -i <netdev>
   driver: unic
   version: 1.0
   firmware-version: 1.0
   expansion-rom-version:
   bus-info: 00002
   supports-statistics: yes
   supports-test: yes
   supports-eeprom-access: no
   supports-register-dump: yes
   supports-priv-flags: no

Register Dumping
----------------
Dump the hardware registers and report the dumpping log through vendor's support
channel using below cmd:

::

   # ethtool -d <netdev>

Performance tuning
==================
For different workload, the interrupt for the driver may have different cpu
pinnig policy, the below cmd can be used to set cpu pinnig policy for unic
driver's ceq (Completion Event Queue) interrupt, which is used to notify
the driver about the arrival of rx packet and completion of tx packet:

::

    # irq_num_list=$(cat /proc/interrupts | grep "ubase$entity_num" | grep ceq)
    # echo $cpu_num > /proc/irq/$irq_num/smp_affinity_list

CPU Intensive Workload
----------------------
It is recommended to pin different cores to unic driver's interrupt and service
process, adjust interrupt coalesce parameters appropriately to limit interrupts
for lower CPU utilization:

::

   # ethtool -C <netdev> rx-usecs XXX tx-usecs XXX

Note, the **max_int_gl** in '/sys/kernel/debug/ubase/$entity_num/unic/caps_info'
is the max value of coalesce parameter.

Latency-sensitive Workload
--------------------------
It is recommended to pin the same core to unic driver's interrupt and service
process, disable unic driver's interrupt coalesce feature to ensure that
interrupt is triggered as soon as possible:

::

   # ethtool -C <netdev> rx-usecs 0 tx-usecs 0

Manage Software
===============
There is a manage software for UB, QOS & object context & IP address which are
related to unic driver depend on the configuration from that manage software,
refer to below debugfs for more detail on the configuration:

::

   QOS: /sys/kernel/debug/ubase/$entity_num/unic/qos/
   Object Context: /sys/kernel/debug/ubase/$entity_num/unic/context
   IP address: /sys/kernel/debug/ubase/$entity_num/unic/ip_tbl/ip_tbl_list

Support
=======
If there is any issue or question, please email the specific information related
to the issue or question to <dev@openeuler.org> or vendor's support channel.
