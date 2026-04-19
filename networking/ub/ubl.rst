.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.

=============
UB Link Layer
=============

Overview
========
The ubl module implements core UB (UnifiedBus) networking functionality that
serves as the foundation for all UB networking device drivers. This module
provides essential UB link protocol handling, device setup utilities, and
standard operations that are shared across different UB networking hardware
implementations.

UB is a new interconnection protocol that defines its own Layer 2 protocol
when integrating into the networking stack for the Linux kernel, see more
in the UB spec <https://www.unifiedbus.com/en>.

The format of a complete UB packet is as follows:
UB Link header (UBL HDR) includes UB LINK, CC and NPI.
UB Network header consists of CC, NPI, and traditional network packet headers.

.. code-block:: none

   <-------- UBL HDR ----------->
   +--------------+------+------+---------+----------+
   |    UB LINK   |  CC  |  NPI | Network |  Payload |
   +--------------+------+------+---------+----------+
                  <------ UB Network ----->

   UB LINK: Data link layer defined by UB protocol.
   CC: Congestion Control.
   NPI: Network Partition Identifier.
   Network: Traditional L3 header, like IPv4, IPv6 or the network control header
            defined in UB.

What the ubl module sees is as follows, as the 'cfg' field is carried through
BD (Buffer Description) for hw to construct UB LINK, the 'sw_ctype' is used
in ubl module, which corresponds to the 'cfg' field defined in UB LINK,
indicating which kind of network packet is encapsulated.

.. kernel-doc:: include/net/ub/ubl.h
   :identifiers: ublhdr

API interface
=============
Before registering `struct net_device` to the networking stack, a UB networking
driver is supposed to allocate and set up a `struct net_device` by calling
alloc_ubldev_mqs().

Before passing a skb to the driver for sending, networking stack will insert the
necessary UB link layer by calling ubl_create_header() through create ops in
struct header_ops.

Also, the driver is supposed to call ubl_type_trans() to set up the skb
correctly when it receives a packet.

.. kernel-doc:: drivers/net/ub/dev/ubl.c
   :identifiers: alloc_ubldev_mqs ubl_create_header ubl_type_trans

An example of using the above API is the unic driver, see more detail using in
:ref:`Documentation/networking/ub/unic.rst`

Technical Discussion
====================
If there is any technical question about UB link layer, please start a technical
discussion by sending a mail to <ub-spec@lists.unifiedbus.com>.
