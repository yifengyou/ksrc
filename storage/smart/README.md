# smart

掌握磁盘 S.M.A.R.T.（Self-Monitoring, Analysis, and Reporting Technology）技术，不仅需要理解其用户空间工具（如 `smartctl`），更要精通其在内核中的实现机制、数据通路、协议转换及与存储子系统的深度集成。

以下是核心技术点：

---

### 一、S.M.A.R.T. 协议基础与 ATA 规范
- **ATA/ATAPI Command Set (ACS)**：  
  熟悉 ACS-4/ACS-5 标准中定义的 S.M.A.R.T. 命令：
    - `SMART READ DATA` (B0h / D5h)
    - `SMART READ ATTRIBUTE VALUES` (D5h)
    - `SMART READ ATTRIBUTE THRESHOLDS` (D6h)
    - `SMART RETURN STATUS` (DAh / B0h)
    - `SMART ENABLE/DISABLE OPERATIONS` (D9h / D8h)
    - `SMART EXECUTE OFF-LINE IMMEDIATE` (D4h)
- **S.M.A.R.T. 数据结构**：  
  掌握 512 字节 `SMART READ DATA` 结构体（含 Vendor Specific 区域），包括：
    - **Attribute Table**（通常前 30 个属性）
    - **Offline Data Collection Status**
    - **Self-test Execution Status**
    - **Error Log Summary**
    - **Self-test Log Directory**

---

### 二、Linux 内核中的 S.M.A.R.T. 实现架构

#### 1. **libata 子系统（核心桥梁）**
- **位置**：`drivers/ata/libata-core.c`, `libata-scsi.c`, `libata.h`
- **关键机制**：
    - 将原生 ATA 命令（如 `READ LOG EXT`）封装为 SCSI CDB（通过 `ata_scsi_queuecmd`）
    - 实现 `HDIO_DRIVE_CMD` ioctl 的处理（`ata_cmd_ioctl`）
    - 支持 **Taskfile I/O** 与 **Packet I/O** 模式
- **重要结构体**：
    - `struct ata_taskfile`：描述 ATA 命令寄存器状态
    - `struct ata_queued_cmd (qc)`：表示一个待执行的 ATA 命令

#### 2. **SCSI 中间层（Mid-layer）集成**
- **位置**：`drivers/scsi/sd.c`（SCSI disk 驱动）
- **作用**：
    - 接收来自块设备层的 `HDIO_DRIVE_CMD` 请求
    - 调用 `scsi_cmd_ioctl` → 最终路由到 `ata_scsi_queuecmd`
    - 处理 **12-byte / 16-byte CDB**（如 `READ LOG EXT` = 0x3E）

#### 3. **块设备层（Block Layer）接口**
- **ioctl 处理**：
    - `blkdev_ioctl` → `scsi_cmd_ioctl` → `sd_ioctl`
    - 关键命令：`HDIO_DRIVE_CMD`, `HDIO_GET_IDENTITY`
- **头文件**：`include/uapi/linux/hdreg.h` 定义了所有 HDIO 命令和数据结构

#### 4. **错误处理与日志**
- **ATA Error Handling (EH)**：  
  libata 的 EH 机制（`ata_eh_recover`）会处理 S.M.A.R.T. 相关错误（如 log read failure）
- **Kernel Log**：  
  通过 `ata_dev_printk` 输出诊断信息（如 “SMART Health Status: Warning”）

---

### 三、NVMe 的 S.M.A.R.T. / Health Log（对比 ATA）
- **命令**：`Get Log Page` with **Log ID = 02h (SMART/Health Information)**
- **内核实现**：
    - `drivers/nvme/host/core.c`：`nvme_get_log` 函数
    - `nvme_ioctl`：处理 `NVME_IOCTL_ADMIN_CMD`
- **数据结构**：`struct nvme_smart_log`（定义于 `include/linux/nvme.h`）
- **关键字段**：`critical_warning`, `temperature`, `available_spare`, `media_errors`

> ⚠️ 注意：NVMe 不使用 S.M.A.R.T. 术语，但功能等价。

---

### 四、安全与权限模型
- **CAP_SYS_RAWIO**：  
  执行 `HDIO_DRIVE_CMD` 需要 `CAP_SYS_RAWIO` 能力（防止普通用户读取敏感数据）
- **Secure Boot / Lockdown**：  
  在 Lockdown Integrity 模式下，可能禁止原始 ATA 命令访问

---

### 五、调试与开发技巧
1. **启用 libata 调试**：
   
   ```bash
   echo 1 > /sys/module/libata/parameters/debug_mask
   ```
2. **跟踪 ATA 命令**：
    - 使用 `blktrace` + `blkparse` 分析 I/O
    - 使用 `ftrace` 跟踪 `ata_scsi_queuecmd`
3. **解析原始 SMART 数据**：
    - 从 `/dev/sda` 读取 512 字节 log（需 root + raw access）
    - 对照 ACS 标准解析字节偏移
4. **自检（Self-test）状态机**：
    - 理解 short/long/offline test 的执行流程
    - 监控 `ata_dev->spdn_cnt`（自检进度）

---

### 六、高级话题

- **S.M.A.R.T. over USB/Thunderbolt**：  
  依赖桥接芯片是否透传 ATA 命令（多数不支持）
- **RAID 控制器支持**：  
  需控制器提供 **Pass-through Mode**（如 MegaRAID 的 `MegaCli -AdpEventLog`）
- **eMMC/UFS 的健康监控**：  
  使用 EXT_CSD（eMMC）或 VU 命令（UFS），非 S.M.A.R.T.
- **内核配置选项**：
    - `CONFIG_ATA_PIIX`, `CONFIG_SATA_AHCI`（驱动）
    - `CONFIG_BLK_DEV_SD`（SCSI disk）
    - `CONFIG_IDE`（旧 IDE，已废弃）

---

### 七、源码阅读重点
| 文件 | 关键函数/结构 |
|------|---------------|
| `drivers/ata/libata-scsi.c` | `ata_scsi_queuecmd`, `ata_cmd_ioctl` |
| `drivers/scsi/sd.c` | `sd_ioctl`, `sd_check_events` |
| `block/scsi_ioctl.c` | `scsi_cmd_ioctl` |
| `include/linux/ata.h` | `ATA_SMART_*` 命令宏定义 |
| `Documentation/ABI/testing/sysfs-block-ata` | 用户可见 sysfs 接口说明 |

---

### 总结

Linux 内核专家对 S.M.A.R.T. 的掌握应达到：
- **能修改 libata 以支持新 S.M.A.R.T. 属性**
- **能调试 SMART 命令在 AHCI/NVMe 控制器上的失败原因**
- **能设计内核模块安全地暴露健康数据给用户空间**
- **理解 S.M.A.R.T. 在虚拟化（如 QEMU virtio-blk）中的限制**

这不仅是“会用 smartctl”，而是能**从硅片到系统调用全程掌控数据流**的能力。
