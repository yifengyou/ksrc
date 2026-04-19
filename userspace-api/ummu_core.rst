.. SPDX-License-Identifier: GPL-2.0+
.. ummu_core:

=======================
UMMU_CORE Userspace API
=======================

The UMMU UAPI provides APIs that enable communication between user-space components
and kernel-space components.The primary use case is Shared Virtual Address (SVA).

.. contents:: :local:

Functionalities
===============
Only kernel-mode process expose the APIs. The supported user-kernel APIs
are as follows:

1. Allocate/Free a TID
2. Send one or more PLBI commands
3. Map or unmap resources, including MAPT block and command queues

Interfaces
==========
Although the data structures defined in UMMU_CORE UAPI are self-contained,
no user-facing API functions are provided. Instead, UMMU_CORE UAPI is
designed to work with UMMU_CORE driver.

Upon loading, the UMMU_CORE driver registers a TID device, and sets up its operation function table.
The supported operations include open, release, map, and ioctl.

Datastructures and Definitions
------------------------------
1. struct ummu_token_info: stores token information for a shared-memory segment.

  - input: specifies the token generation mode.If input is 0, the tokenVal field is used as the token value.
           If input is 1, the UMMU library generates a random token value, and tokenVal is ignored.
  - tokenVal: the token value to use when input is 0.

2. enum ummu_mapt_perm: access permissions for a shared-memory segment

  - MAPT_PERM_W: write only
  - MAPT_PERM_R: read only
  - MAPT_PERM_RW: read and write
  - MAPT_PERM_ATOMIC_W: atomic write only
  - MAPT_PERM_ATOMIC_R: atomic read only
  - MAPT_PERM_ATOMIC_RW: atomic read and write

3. enum ummu_mapt_mode: Memory Address Permission Table mode

  - MAPT_MODE_ENTRY: only one memory address segment can be managed per TID.
  - MAPT_MODE_TABLE: multiple memory address segments can be managed per TID.

4. enum ummu_ebit_state:

  - UMMU_EBIT_OFF: disable ebit check
  - UMMU_EBIT_ON: enable ebit check

5. definitions:

  - TID_DEVICE_NAME: a character device that enables user-mode processes to interact
                     with hardware or software through system calls.
  - UMMU_IOCALLOC_TID: operation code for allocating a TID.
  - UMMU_IOCFREE_TID: operation code for freeing a TID.
  - UMMU_IOCPLBI_VA: operation code to flush the PLB cache for a specific virtual address.
  - UMMU_IOCPLBI_ALL: operation code to flush the PLB cache for all virtual addresses.

Descriptions and Examples
-------------------------
1. allocate/free tid

The input parameters is *struct ummu_tid_info*.Below is an example:
::
    struct ummu_tid_info info = {};

    int fd = open("/dev/ummu/tid", O_RDWR | O_CLOEXEC);
    ioctl(fd, UMMU_IOCALLOC_TID, &info);
    ioctl(fd, UMMU_IOCFREE_TID, &info);

The PLBI command operation is performed via the ioctl interface,
using the operation codes UMMU_IOCPLBI_VA or UMMU_IOCPLBI_ALL.

2. map resources

This interface is used in two scenarios:
(1) Creating a new MAPT block
(2) Initializing user-mode queues

For example:
::
    mmap(NULL, size, prot, flags, fd, PA);

On success, this returns a virtual address.

3. unmap resources

This interface is used in two scenarios:
(1) Clearing MAPT blocks
(2) When user-mode process exits, all associated MAPT blocks and use-mode queue resources
are cleared.

For example:
::
    munmap(buf, BLOCK_SIZE_4K);
