.. SPDX-License-Identifier: GPL-2.0+

Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.

============================
CDMA Driver Support Library
============================

Overview
=========
CDMA (Crystal Direct Memory Access) is used to provide asynchronous memory read
and write operations between hosts or between host and devices.

The key features are described as follows:

+ 1. Peer-to-peer communication between hosts, enabling bidirectional asynchronous memory read or write.
+ 2. Asynchronous memory read and write between host and devices via DMA.
+ 3. Asynchronous memory read and write between devices and host via DMA.

This document aims to provide a guide for device driver developers on the CDMA
driver API, as well as how to use it for asynchronous memory read and write
operations between hosts in CDMA.

CDMA Interface Operation
==========================
The API of the CDMA framework does not support arbitrary concurrent calls.
For example, using a Queue object and destroying the Queue concurrently can lead
to unexpected exceptions.
Users are required to ensure the correctness of the call logic. These objects
include context, segment, queue, etc.

.. kernel-doc:: include/ub/cdma/cdma_api.h
   :functions:

.. kernel-doc:: drivers/ub/cdma/cdma_api.c
   :export:

CDMA API Sample
=================

DMA Resource Sample
-----------------------
.. code-block:: c

   #define POLL_LOOP_EXAMP 100
   #define POLL_MSLEEP_EXAMP 1
   #define QUEUE_DEPTH_EXAMP 512
   #define QUEUE_RMT_EID_EXAMP 2
   #define QUEUE_DCAN_EXAMP 1

   struct dma_seg_cfg local_seg_cfg = {};
   struct dma_seg_cfg rmt_seg_cfg = {};
   struct dma_seg *local_seg, *rmt_seg;
   struct queue_cfg queue_cfg = {};
   int ctx_handle, queue_handle;
   struct dma_device *dev_list;
   struct dma_device *dma_dev;
   u32 loop = POLL_LOOP_EXAMP;
   struct dma_cr ret_cr = {};
   dma_status status;
   int ret = -EINVAL;
   u32 dev_num = 0;

   dev_list = dma_get_device_list(&dev_num);
   if (!dev_list || !dev_num) {
      printk("get device list failed\n");
      return;
   }
   dma_dev = &dev_list[0];

   ctx_handle = dma_create_context(dma_dev);
   if (ctx_handle < 0) {
      printk("create context failed, ctx_handle: %d.\n", ctx_handle);
      goto free_dev_list;
   }

   queue_cfg.queue_depth = QUEUE_DEPTH_EXAMP;
   queue_cfg.rmt_eid.dw0 = QUEUE_RMT_EID_EXAMP;
   queue_cfg.dcna = QUEUE_DCAN_EXAMP;
   queue_handle = dma_alloc_queue(dma_dev, ctx_handle, &queue_cfg);
   if (queue_handle < 0) {
      printk("allocate queue failed, queue_handle: %d.\n", queue_handle);
      goto delete_ctx;
   }

   /* Input parameter, local payload address */
   local_seg_cfg.sva = (u64)local_buf_addr;
   /* Input parameter, local payload memory length */
   local_seg_cfg.len = local_buf_len;

   local_seg = dma_register_seg(dma_dev, ctx_handle, &local_seg_cfg);
   if (!local_seg) {
      printk("register local segment failed.\n");
      goto free_queue;
   }

   /* Input parameter, remote payload address */
   rmt_seg_cfg.sva = (u64)rmt_buf_addr;
   /* Input parameter, remote payload memory length */
   rmt_seg_cfg.len = rmt_buf_len;

   rmt_seg = dma_import_seg(&rmt_seg_cfg);
   if (!rmt_seg) {
      printk("import rmt segment failed.\n");
      goto unregister_seg;
   }

   status = dma_write(dma_dev, rmt_seg, local_seg, queue_handle);
   if (status != DMA_STATUS_OK) {
      printk("write failed, status = %d.\n", status);
      goto unimport_seg;
   }

   while (loop > 0) {
      ret = dma_poll_queue(dma_dev, queue_handle, 1, &ret_cr);
      if (ret == 1)
         break;
      msleep(POLL_MSLEEP_EXAMP);
      loop --;
   }
   ...

   unimport_seg:
      dma_unimport_seg(rmt_seg);
   unregister_seg:
      dma_unregister_seg(dma_dev, local_seg);
   free_queue:
      dma_free_queue(dma_dev, queue_handle);
   delete_ctx:
      dma_delete_context(dma_dev, ctx_handle);
   free_dev_list:
      dma_free_device_list(dev_list, dev_num);
   ...

/* Register the virtual kernel online interface to notify users that
 * the kernel-mode CDMA driver is online.
 */
DMA Client Sample
-------------------

.. code-block:: c

   /* After the driver is loaded or restarted upon reset, the add
    * interface is called to allow users to request resources
    * required for DMA.
    */
   static int example_add(u32 eid)
   {
      /* Refer to DMA Resource Sample, create context, queue, segment
       * dma_get_device_list, dma_create_context, dma_alloc_queue etc.
       */
      return 0;
   }

   /* The stop interface is used to notify users to stop using the
    * DMA channel.
    */
   static void example_remove(u32 eid)
   {
      /* Refer to DMA Resource Sample, delete context, queue, segment
       * dma_free_queue dma_delete_context dma_free_device_list etc.
       */
   }

   /* The remove interface is used to notify users to delete resources
    * under DMA.
    */
   static void example_stop(u32 eid)
   {
      /* Stop read and write operations through status control */
   }

   static struct dma_client example_client = {
      .client_name = "example",
      .add = example_add,
      .remove = example_remove,
      .stop = example_stop,
   };

   static void example_register_client(u32 eid)
   {
      ...
      dma_register_client(&example_client);
      ...
   }

Support
========
If there is any issue or question, please email the specific information related
to the issue or question to <dev@openeuler.org> or vendor's support channel.
