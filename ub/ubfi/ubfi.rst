.. SPDX-License-Identifier: GPL-2.0

===========
UBFI Driver
===========

What is UBFI
============

When BIOS boots the OS with UB firmware, it should report the UB-related
information in the system so that the OS can obtain the UB-related information,
including UBC, UMMU, and all other information required for UB enabling.

Startup information is related to chip specifications and is static information
that can be reported through a static information table. There are three
reporting methods: UBIOS, ACPI, and Device Tree. The only difference among these
three methods lies in the entry points for obtaining the UB-related information
tables. The contents of each information table remain consistent.

UnifiedBus Firmware Interface (UBFI) driver supports obtaining UB-related
information from the BIOS via the ACPI table or device tree. And create software
instances of UBCs and UMMUs in the OS.

UBFI driver is one of the fundamental drivers of UB. It has achieved the
aforementioned functions.

.. code-block:: none

      +--------------------------------------------------------------+
      |                             BIOS                             |
      +--------------------------------------------------------------+
              ^                                              ^
              |acpi                                        of|
              v                                              v
      +--------------------------------------------------------------+
      |                            kernel                            |
      +--------------------------------------------------------------+
                                      ^
                                      |
                                      v
      +--------------------------------------------------------------+
      |                             ubfi                             |
      +--------------------------------------------------------------+
              ^                                              ^
              |                                              |
              v                                              v
      +-----------------+                          +-----------------+
      |      ubus       |                          |      ummu       |
      +-----------------+                          +-----------------+

What does UBFI do
=================

When loading the ubfi driver, it detects the current OS boot mode and retrieves
the UBRT (UB root table) physical address from the BIOS.

  - ACPI (UBRT table)
  - device tree (node: chosen: ubios-information-table)

For the structure of UBRT, please refer to https://www.unifiedbus.com/

Create UBC
----------

BIOS may report information about multiple UBCs, some of which is shared among
multiple UBCs and is reported in ``struct ubrt_ubc_table``

.. kernel-doc:: drivers/ub/ubfi/ubc.h
   :functions: ubrt_ubc_table

As ``ubc_cna_start``, ``ubc_cna_end``, ``ubc_eid_start``, ``ubc_eid_end``,
``ubc_feature``, ``cluster_mode``, these attributes belong to the entire UBPU
node and are shared by all UBCs.

For a single UBC, its information is reported in the ``struct ubc_node``

.. kernel-doc:: drivers/ub/ubfi/ubc.h
   :functions: ubc_node

We have performed the following work on a single UBC.

 - Create the UBC structure and record the UBC information
 - Register the UBC irq with the kernel
 - Initialize UBC and register the UBC device with the kernel
 - Register the MMIO address space of UBC with the kernel
 - Set the MSI domain for all UBCs

After completing these steps, ``struct list_head ubc_list`` will be provided
externally, which records all UBCs within the node for subsequent
interconnection and communication purposes.

Set MSI domain for UBC
~~~~~~~~~~~~~~~~~~~~~~

UBFI driver requests interrupts from the interrupt management subsystem on
behalf of the entity and delivers the interrupt configuration to the entity.
When reporting an interrupt, the entity writes the interrupt information into
the interrupt controller, which then calls back the interrupt management
subsystem. The interrupt management subsystem subsequently invokes the UB driver
to handle the corresponding interrupt.

UB created a new Message Signaled Interrupt domain called USI (UB Signaled
Interrupt).

UB will add a platform device in the DSDT and IORT tables to associate UBC
with the USI domain. If booting with device tree, we will add a new UBC node in
DTS for binding the USI domain. For each UBC, a corresponding number of platform
devices should be created. We will set the USI domain of these platform devices
to the USI domain of each UBC.

Example in DTS for UBC::

  ubc@N {
      compatible = "ub,ubc";
      #interrupt-cells = <0x3>;
      interrupt-parent = <0x01>;
      interrupts = <0x0 0xa 0x4>;
      index = <0x00>;
      msi-parent = <0x1 0xabcd>;
  };

Parse UMMU and PMU
------------------

Both UMMU and UMMU-PMU devices are platform devices and support creation via
ACPI and DTS.

ACPI method:
  - The device information for UMMU and UMMU-PMU has been added to DSDT and
    IORT tables.
  - When the OS enables ACPI functionality, the ACPI system will recognize
    the device information in the DSDT and IORT tables and automatically
    create platform devices for UMMU and UMMU-PMU.
  - The number of platform devices for UMMU and UMMU-PMU depends on the
    number of device information nodes described in the DSDT and IORT tables.

DTS method:
  - The DTB file has added device tree nodes for UMMU and UMMU-PMU.
  - When the OS enables the device tree functionality, the DTS system will
    recognize the device tree nodes for UMMU and UMMU-PMU, and then
    automatically create platform devices for them.
  - The number of platform devices for UMMU and UMMU-PMU depends on the
    number of corresponding device tree nodes described in the device tree.

  Example in DTS for UMMU and UMMU-PMU::

    ummu@N {
        compatible = "ub,ummu";
        index = <0x0>;
        msi-parent = <&its>;
    };

    ummu-pmu@N {
        compatible = "ub,ummu_pmu";
        index = <0x0>;
        msi-parent = <&its>;
    };

Obtain UMMU nodes from the UBRT table:
  - The UBRT table can be parsed to extract the UMMU sub-table, which contains
    several UMMU nodes. Each UMMU node describes the hardware information of an
    UMMU device and its corresponding UMMU-PMU device. The specific content of
    UMMU nodes can be found in ``struct ummu_node``.

  - The number of UMMU platform devices created via ACPI or DTS should match the
    number of UMMU nodes in the UBRT table, as they have a one-to-one
    correspondence. The same one-to-one correspondence applies to UMMU-PMU
    devices and UMMU nodes.

Configure UMMU and PMU devices:
  - For each UMMU node parsed from the UBRT table, the register information and
    NUMA affinity described in the UMMU node can be configured for the
    corresponding UMMU and UMMU-PMU devices.
  - Each UMMU node's content is stored in the ``ubrt_fwnode_list`` linked list.
    Subsequently, the corresponding UMMU node can be found by using the fwnode
    property of the UMMU and UMMU-PMU devices, making it convenient to obtain the
    hardware information during the initialization of the UMMU and UMMU-PMU
    drivers.