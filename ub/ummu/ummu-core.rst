.. SPDX-License-Identifier: GPL-2.0+

======================================
UMMU-CORE
======================================

:Authors: - Yanlong Zhu <zhuyanlong3@huawei.com>

Introduction
============
The Unified Bus Memory Management Unit (abbreviated as UMMU) is a component
that provides memory address mapping and access permission verification
during memory access processes.
It supports the sharing of memory resources between UBPU (UB Processing Units)
and ensures legitimate access to memory.

The UMMU-Core is designed to work with the Linux IOMMU framework, as an
extension, providing the necessary interfaces to integrate with the system.
To maintain flexibility in deployment, the UMMU-Core can be compiled as a
loadable kernel module or built-in kernel image.

EID Management
--------------

UMMU uses the following inputs — DstEID, TokenID and UBA (Unified Bus Address) —
to determine whether the entity is valid and which address domain it should access.

Every UB entity must register its EID (Entity ID) with the UB domain to
communicate with other entities. UMMU-Core provides :c:func:`ummu_core_add_eid()`
and :c:func:`ummu_core_del_eid()` functions to manage EID.

In some cases, UB devices may register before all UMMU devices. To handle
this, we designed an EID cached list to temporary save EIDs. Upon an UMMU
device register as global core device, the UMMU-Core will flushes the EID
cached list to it. Thread safety is guaranteed by the UMMU-Core. For
detailed information, refer to the `UB-Base-Specification-2.0`_.

.. _UB-Base-Specification-2.0: https://www.unifiedbus.com/

TokenID Management
------------------

Each UB entity has multiple address spaces, such as DMA space, SVA space,
and others. The TokenID identifies the address space associated with each entity.

The UMMU-Core introduces tdev (TID Device), a pseudo-device used to abstract
the concept of TID. It also supports UMMU driver functionality, enabling driver
management. The tdev can be used to allocate and grant memory address spaces.
When tdev is released, all associated resources will be freed.

UMMU-Core acts as the TID manager in the UB system, offering TID allocation
strategies and TID allocation APIs to the UMMU driver.

UMMU-Core supports multiple TID allocation strategies:

- TRANSPARENT:
   The TID is compatible with the global PASID (Process Address Space ID),
   enabling seamless integration with system-wide address space management.
- ASSIGNED:
   A pre-allocated TID, assigned from an external framework or management system.
- NORMAL:
   The default TID allocation strategy, suitable for the majority of use cases.

UMMU Device Registration
------------------------

The UMMU device registration is performed in two steps. An UMMU device
must implement the `ummu_core_device` interface and initialize it using
:c:func:`ummu_core_device_init()` function. This function initializes
the core device and allocates a dedicated TID manager to handle TID operations.

Multiple UMMU devices can register to UMMU-Core by :c:func:`ummu_core_device_register()`
function. However, only global core device can take the charge of all UB device requests,
such as :c:func:`add_eid()` and :c:func:`del_eid()` functions.

.. code-block:: none

                                        +-------------------+
                                        |  IOMMU Framework  |
                                        +---------+---------+
                                                  |
                                       +----------+---------+
                                       | Global Core Device |
                                       +----------+---------+
                                                  |
             +------------------------+-----------+-----------+------------------------+
             |                        |                       |                        |
    +-------------------+   +-------------------+   +-------------------+     +-------------------+
    | Core Device 0     |   | Core Device 1     |   | Core Device 2     | ... | Core Device x     |
    +-------------------+   +-------------------+   +-------------------+     +-------------------+

Support KSVA mode
-----------------

The KSVA (Kernel-space Shared Virtual Addressing) is not supported in the
current IOMMU framework, as it maps the entire kernel address space to
devices, which may cause critical errors.

By leveraging isolated address space IDs and fine-grained permission controls,
we can restrict each device only access to the authorized address space
with KSVA mode.

To manage the access permissions of each PASID, the IOMMU can implement a
permission checking mechanism. We abstract the permission management
operations into four fundamental types:

- grant:
  Grant access to a specified memory address range with defined
  permissions (e.g., read, write, execute).
- ungrant:
  Revoke previously granted access to a memory address range, invalidating
  the device's permissions for that region.
- plb_sync_all:
  Synchronize the PLB (Permission Lookaside Buffer) for all registered
  PASIDs, ensuring global consistency of permission state across the IOMMU.
- plb_sync:
  Synchronize the PLB for a specific PASID and memory range, minimizing
  latency while maintaining access control integrity.

These operations are integrated into the `iommu_domain` as part of the
`iommu_perm_ops` interface.

UMMU SVA maintains a set of permission tables and page tables for each TID.
These resources can be allocated via the :c:func:`alloc_tid()` operation.
Once a TID is assigned, read and write permissions for the specific virtual
memory address ranges can be granted or ungranted.

To access granted memory address ranges, permission verification is required.
