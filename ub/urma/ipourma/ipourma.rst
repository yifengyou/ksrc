.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

===============================================
IP over URMA (IPoURMA)
===============================================
Overview
==========
IP over URMA(abbreviated as IPoURMA) is a Linux kernel-level adapter driver that sits between the TCP/IP stack and the UB (UnifiedBus Protocol).

By encapsulating the UB protocol into a virtual Ethernet device, IPoURMA enables seamless integration between UB and the standard TCP/IP stack. Users can leverage the full power of the standard socket API to communicate over UB—without modifying existing applications.

Context
=========
The IPoURMA driver operates at the kernel level, serving as a protocol adapter between the traditional TCP/IP protocol stack and the UB framework.

.. code-block:: code

 +-------------------------------------+
 |              +-------+------------+ |
 |              |       |     APP    | |
 |              |       +------------+ |
 |              |User   | Socket API | |
 |              +-------+------------+ |
 |              |       |TCP/IP Stack| |
 |              |       +------------+ |
 |              |       |   IPoURMA  | |
 |              |       +------------+ |
 |              |       |   UBCORE   | |
 |              |       +------------+ |
 |  Software    |Kernel |    UDMA    | |
 +--------------+-------+------------+ |
 |               Hardware              |
 +-------------------------------------+


As illustrated in the context diagram:

* On the upper layer, IPoURMA integrates with the standard TCP/IP stack, exposing an Ethernet-like interface. This allows applications to communicate over the UB network using the familiar socket API.
* On the lower layer, IPoURMA interfaces with UBCORE, the core module of URMA (Unified Remote Memory Access). UBCORE provides foundational capabilities such as memory registration, remote access control, and memory protection.
* URMA, as a key component of the UB protocol stack, is designed to abstract and facilitate communication between different hardware and software entities.
* Underlying the entire stack, the UDMA (UnifiedBus Direct Memory Access) is a hardware I/O device that provides direct memory access capabilities.