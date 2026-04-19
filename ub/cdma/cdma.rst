.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.

======================================
Crystal Direct Memory Access (CDMA)
======================================

Overview
=========
CDMA (Crystal Direct Memory Access) is used to provide asynchronous memory read
and write operations between hosts or between host and devices.

The key features are described as follows:

+ 1. Peer-to-peer communication between hosts, enabling bidirectional asynchronous memory read or write.
+ 2. Asynchronous memory read and write between host and devices via DMA.
+ 3. Asynchronous memory read and write between devices and host via DMA.

Overall Structure
===================

Driver Modules
---------------

The CDMA driver is divided into three modules: UBASE, K-DMA, and U-DMA:

.. code-block:: none

            +---------------------------+
            |            APP            |
            +---------------------------+
                        |
            +---------------------------+
            |           U-DMA           |
            +---------------------------+
               |               |
               |     +-------------------+
               |     |       K-DMA       |
               |     +-------------------+
               |          |           |
               |  +----------------+  |
               |  |  Auxiliary Bus |  |
               |  +----------------+  |
               |          |           |
               |     +-------------------+
               |     |       UBASE       |
               |     +-------------------+
               |                      |
            +---------------------------+
            |        CDMA Hardware      |
            +---------------------------+

+ Figure 1: CDMA Module Relationship Diagram

UBASE provides management of hardware public resources, including CMD, mailbox
management, event management, and device reset.
It also provides a device and driver matching interface for the CDMA driver based
on the kernel auxiliary bus.

Within the K-DMA module, functional blocks are divided according to different data
objects: Device Management is responsible for device attribute configuration
(such as EID, UPI, etc.) and device capability queries (such as Jetty specifications);
Event Management handles events reported by the controller, including completion
events and asynchronous events;
Queue Management is responsible for JFS(Jetty For Send)/JFC(Jetty For Completion)
resource management.

Within the U-DMA module, functional blocks are divided according to data plane
functions: Memory verbs, which are unidirectional operations including read,
write, and atomic operations.
Event verbs register callback functions with K-DMA for post-processing of
asynchronous events.

Interaction Timing
-------------------

.. code-block:: none

    +---------+ +---------+ +---------+  +---------+ +---------+ +---------+
    |   APP   | |  U-DMA  | |  K-DMA  |  |  UBASE  | |    MS   | |    HW   |
    +---------+ +---------+ +---------+  +---------+ +---------+ +---------+
        | CDMA API  |           |            |           |           |
        |---------->|  ioctl    |            |           |           |
        |           |---------->|  UBASE Func|           |           |
        |           |           |----------->|           |           |
        |           |           |<-----------|           |           |
        |           |           |         HW Interface   |           |
        |           |           |----------------------------------->|
        |           |           |<-----------------------------------|
        |           |           | UBASE Func |           |           |
        |           |           |----------->|  MS MSG   |           |
        |           |           |            |---------->|           |
        |           |           |            |<----------|           |
        |           |           |<-----------|           |           |
        |           |<----------|            |           |           |
        |<----------|           |            |           |           |
        |           |           |            |           |           |
        | CDMA API  |           |            |           |           |
        |---------->|  HW Interface          |           |           |
        |  DMA OPT  |----------------------------------------------->|
        |           |<-----------------------------------------------|
        |<----------|           |            |           |           |
        |           |           |            |           |           |

+ Figure 2: CDMA Interaction Timing

The 'Figure 2' shows the runtime sequence of interactions between the CDMA driver,
the UBASE driver, the MS(Management Software), and the hardware.

Functionality
===============

CDMA device creation and reset
---------------------------------
The CDMA devices are dynamically created by the resource management on the
management software, and the reset operation is also performed by the management
software.
Files involved: cdma_main;

CDMA device and context management
------------------------------------
The CDMA driver supports lifecycle management of CDMA devices and enables
applications to create device contexts based on these devices.
Files involved: cdma_context, cdma_main;

CDMA queue management
---------------------------
The CDMA queue includes the CDMA JFS and JFC defined on the chip, and encompasses
the management of JFS, JFC, and CTP(Compact Transport) resources.
When a remote memory read/write request is initiated, the JFS is used to fill the
corresponding WQE(Work Queue Entry), and the request execution result is received
through the JFC.
Files involved: cdma_queue, cdma_jfs, cdma_jfc, cdma_tp, cdma_db;

CDMA segment management
-----------------------------
The CDMA driver uses local and remote segment resources for read and write operations.
These operations primarily include the register and unregister functions for
local segment resources, as well as the import and export functions for remote
segment resources.
Files involved: cdma_segment;

CDMA read/write semantics
---------------------------
The CDMA communication capability is implemented on the chip side as CTP mode
communication, supporting transaction operations including:
write, write with notify, read, CAS(Compare And Swap), and FAA(Fetch And Add).
Files involved: cdma_handle;

Processing and reporting of EQE events
---------------------------------------
The CDMA communication device supports the reporting of transaction operation
results in interrupt mode. The reported events are classified into two types:
CE(Completion Event) and AE(Asynchronous Event).
The two types of events trigger the event callback processing function registered
by the CDMA driver in advance in the interrupt context.
Files involved: cdma_event, cdma_eq;

Supported Hardware
====================

CDMA driver supported hardware:

===========   =============
Vendor ID       Device ID
===========   =============
0xCC08         0xA003
0xCC08         0xA004
0xCC08         0xD804
0xCC08         0xD805
===========   =============

You can use the ``lsub`` command on your host OS to query devices.
Below is an example output:

.. code-block:: shell

   <Entity Number> Class <000X>: Device <Vendor ID>:<Device ID>
   <00004> Class <0002>: Device <cc08>:<a003>

Debugging
==========

Device Info
-----------

.. code-block:: none

   Query CDMA device information.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/resource_info/dev_info
   The 'CDMA_ENO' value represents the ENO (Entity Number) information for
   CDMA devices. You can use the 'lsub' command on your host OS to query devices.

Capability Info
----------------

.. code-block:: none

   Query CDMA device capability information.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/resource_info/cap_info

Queue Info
-----------

.. code-block:: none

   Query current queue configuration information.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/resource_info/queue_info
   Set the queue ID value for the current query using 'queue_id' command, like
   $ echo 0 > /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/resource_info/queue_id.

Reset Info
------------

.. code-block:: none

   Query CDMA device reset operation records.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/reset_info

JFS Context
--------------

.. code-block:: none

   Query the current JFS channel context information on the software side.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/context/jfs_context
   The channel ID is configured by setting the queue ID command, like
   $ echo 0 > /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/context/queue_id.

JFS Context HW
---------------

.. code-block:: none

   Query the current JFS channel context information on the hardware side.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/context/jfs_context_hw

JFC Context
---------------

.. code-block:: none

   Query the current channel JFC context information on the software side.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/context/sq_jfc_context

JFC Context HW
------------------

.. code-block:: none

   Query the current JFC channel context information on the hardware side.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/context/sq_jfc_context_hw

JFS Entity PI
------------------

.. code-block:: none

   Set or query the PI value of the current JFS channel, used for querying
   specific SQE information of the JFS.
   Example:
      $ echo 0 > /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/entry_info/entry_pi
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/entry_info/entry_pi

JFS Entity Info
----------------

.. code-block:: none

   Query the information of a specific SQE for the current channel JFS.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/entry_info/sqe
   The channel ID is configured through the queue ID command.
   The SQE ID is set by configuring the 'entry_pi' as described above.
   Supports kernel-space resources only.

JFC Entity CI
----------------

.. code-block:: none

   Set or query the CI value of the current JFC channel, used for querying
   specific CQE information of the JFC.
   Example:
      $ echo 0 > /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/entry_info/entry_ci
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/entry_info/entry_ci

JFC Entity Info
----------------

.. code-block:: none

   Query the information of a specific CQE for the current channel JFC.
   Example:
      $ cat /sys/kernel/debug/ubase/<CDMA_ENO>/cdma/entry_info/cqe
   The channel ID is configured through the Queue ID command.
   The CQE ID is set by configuring the 'entry_ci' as described above.
   Supports kernel-space resources only.

Support
========
If there is any issue or question, please email the specific information related
to the issue or question to <dev@openeuler.org> or vendor's support channel.