.. SPDX-License-Identifier: GPL-2.0

======================
UB FWCTL Kernel Design
======================

Overview
========

UB_FWCTL: Auxiliary bus device driver based on PMU IDEV.
It isolates user-mode debug (operation and maintenance information) functions from chip implementation details,
converts user debug commands into CMDQ commands, and sends them to the
software through the CMDQ channel of the PMU IDEV device to implement debug functions.

Description of the Design
=========================

The public debug tool, namely the newly added ub_fwctl tool in this document,
is primarily designed to provide functions such as querying UB public function configurations,
querying the status and statistics of many modules, and querying die-level information.

The debug functions provided by this module are shared among multiple subsystems of UB and are not suitable
for being included in any single feature. Ub_fwctl interfaces with the open-source fwctl framework and
provides a user-defined command format for UB, supporting the public DFX functions of the UB system.

Currently, ub_fwctl only provides querying functions and does not support configuration functions.
The DFX tools for each feature are described in detail in the corresponding feature design documents.
This design document focuses on the design of the ub_fwctl tool::

    Purpose: As Auxiliary device driver, it provides the specific implementation of debug functions
    OPS as provided by the fwctl module, and calls the CMDQ interface to pass debug messages to the software.

    Function List:
        1) Serve as an Auxiliary device driver to match Auxiliary devices.
        2) Register the fwctl device and the specific function implementation of ub_fwctl.
        3) Provide CMD queue management interfaces.

Data structure design of UB FWCTL
=================================

.. kernel-doc:: drivers/fwctl/ub/ub_common.h


System Function Design Description
==================================

Loading and unloading the ub_fwctl driver
-----------------------------------------

Feature Introduction::

        FWCTL is a debug framework scheduled for integration into the mainline Linux kernel.
        It provides a command pathway from userspace to kernelspace,
        requiring device manufacturers to implement their
        own driver plugins registered with the FWCTL kernel framework.
        UB has implemented a driver called ub_fwctl, which consists of both a userspace
        command-line tool (ubctl) and a kernel-space driver (ub_fwctl). After loading the ub_fwctl driver,
        the sysfs system exposes a device file (such as /dev/ubcl) in the OS's /dev directory.
        The userspace program ubctl can then open this device file via open(/dev/ubcl)
        to obtain a file descriptor, and subsequently communicate with the driver through ioctl calls.

Implementation Method of Function::

        1. Ub_fwctl registers itself with the fwctl framework.
        2. As a secondary device, ub_fwctl connects to ubase through the secondary
        bus and uses the CMDQ (command queue) of The PMU IDEV to call the software
        programming interface for reading and writing registers.
        3. ubctl provides command-line commands for users to invoke.
        During operation, ubctl first opens the /dev/fwctl/fwctlNN device file.
        It then assembles a corresponding data structure based on user input.
        Next, it invokes the ioctl() system call to enter kernel mode.
        Upon receiving a command from ubctl, the ub_fwctl driver first validates the command.
        It then communicates with the ubase software module by calling its interface to access the CMDQ.
        The software returns the register access result to ub_fwctl via the CMDQ.
        ub_fwctl subsequently returns this data to user space.
        Finally, after completing its operation, ubctl closes the opened /dev/ubcl file descriptor.

.. code-block:: none

        +-------+            +----------+                     +-------+           +-----+
        | ubctl | --ioctl--> | ub_fwctl | --ubase_send_cmd--> | ubase | --cmdq--> | imp |
        +-------+            +----------+                     +-------+           +-----+

Querying UB link and chip info by ub_fwctl
-----------------------------------------

Feature Introduction::

        After a failure occurs in the production environment,
        further troubleshooting is required to identify the root cause,
        including information checks such as abnormal interrupts, statistical counters, key FIFO status,
        and key state machine status. The ubctl needs to support users to query the chip's
        debug information through the command-line tool and output the chip's debug information
        in a form that is understandable to users.

Implementation Method of Function::

        ubctl receives input from the command line, assembles it into corresponding commands,
        and invokes ioctl to enter kernel space. The fwctl driver copies the data into the kernel space,
        assembles it into the corresponding opcode, and sends the command to the software for processing via
        the CMDQ of the PMU IDEV. After reading the corresponding registers according to the chip's rules,
        the software returns the data to ub_fwctl, which then returns the data to user space.
        Finally, ubctl displays the data.

        The following types of registers are supported for query:
        1. Querying information about the UB link.
        2. Querying QoS memory access information.
        3. Querying port link status.
        4. Querying DL layer service packet statistics.
        5. Querying NL layer service packet statistics.
        6. Querying SSU packet statistics.
        7. Querying BA layer packet statistics.
