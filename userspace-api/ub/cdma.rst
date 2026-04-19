.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.

===============================
CDMA Userspace Support Library
===============================

Overview
=========
CDMA (Crystal Direct Memory Access) is used to provide asynchronous memory read
and write operations between hosts or between host and devices.

The key features are described as follows:

+ 1. Peer-to-peer communication between hosts, enabling bidirectional asynchronous memory read or write.
+ 2. Asynchronous memory read and write between host and devices via DMA.
+ 3. Asynchronous memory read and write between devices and host via DMA.

Char Device
=============
The driver creates one char device per CDMA found on the physical device.
Char devices can be found in /dev/cdma/ and are named as:
/dev/cdma/dev.<index>

User API
=========

ioctl
------
=========================  ====================================================
CDMA_CMD_QUERY_DEV_INFO     Query CDMA device information.
CDMA_CMD_CREATE_CTX         Create user context resource.
CDMA_CMD_DELETE_CTX         Delete user context resource.
CDMA_CMD_CREATE_CTP         Create CTP(Compact Transport) channel resource.
CDMA_CMD_DELETE_CTP         Delete CTP channel resource.
CDMA_CMD_CREATE_JFS         Create JFS(Jetty For Send) resource.
CDMA_CMD_DELETE_JFS         Delete JFS resource.
CDMA_CMD_REGISTER_SEG       Register local segment resource.
CDMA_CMD_UNREGISTER_SEG     Unregister local segment resource.
CDMA_CMD_CREATE_QUEUE       Create queue resource.
CDMA_CMD_DELETE_QUEUE       Delete queue resource.
CDMA_CMD_CREATE_JFC         Create JFC(Jetty For Completion) resource.
CDMA_CMD_DELETE_JFC         Delete JFC resource.
CDMA_CMD_CREATE_JFCE        Create JFCE(Jetty For Completion Event) resource.
=========================  ====================================================

Support
========
If there is any issue or question, please email the specific information related
to the issue or question to <dev@openeuler.org> or vendor's support channel.