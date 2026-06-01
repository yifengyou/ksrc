# smartctl基本用法

掌握 `smartctl` 不仅要会基本用法，更要深入理解其与内核子系统（如 libata、SCSI、NVMe）的交互机制、协议细节、调试能力以及在复杂环境（如 RAID、虚拟化、容器）下的行为。以下是专家级应掌握的 `smartctl` 用法详解：

---

### 一、基础但必须精准掌握的核心命令

| 命令 | 作用 | 专家关注点 |
|------|------|-----------|
| `smartctl -i /dev/sdX` | 显示设备基本信息（型号、序列号、固件、SMART 支持状态） | 检查 `SMART support is: Available - device has SMART capability.` 是否启用 |
| `smartctl -H /dev/sdX` | 获取整体健康自检结果（PASSED/FAILED） | **这是最权威的健康判断**，由固件内部逻辑决定 |
| `smartctl -A /dev/sdX` | 显示所有 S.M.A.R.T. 属性（Attribute Table） | 关注 RAW_VALUE（原始值），而非 normalized value；识别关键属性 ID（5, 197, 198） |
| `smartctl -a /dev/sdX` | 显示全部信息（等价于 `-i -H -A -l error -l selftest`） | 日常诊断首选 |

> 💡 **注意设备命名**：
> - 直通磁盘：`/dev/sda`, `/dev/nvme0n1`
> - RAID 控制器后：需使用 `/dev/sgN`（SCSI generic device）

---

### 二、高级用法：内核专家必须掌握的场景

#### 1. **绕过块设备层，直接通过 SCSI Generic (sg) 接口访问**
当磁盘被 RAID 卡或 HBA 管理时，`/dev/sdX` 可能无法传递原始 ATA 命令。

```bash
# 列出所有 sg 设备
ls -l /dev/sg*

# 通过 sg 设备查询（常见于 MegaRAID、LSI）
sudo smartctl -d megaraid,N -a /dev/sg0   # N 是物理磁盘 ID
sudo smartctl -d sat -a /dev/sg1          # SAT (SCSI_ATA_TRANSLATION) 桥接
```

- **`-d TYPE` 参数是关键**：
    - `sat`：SCSI to ATA Translation（用于 USB/SATA 桥接器）
    - `nvme`：强制 NVMe 模式
    - `megaraid,N`：LSI MegaRAID 控制器
    - `cciss,N`：HP Smart Array

> 📌 **内核关联**：这直接调用 `scsi_cmd_ioctl` → `sd_ioctl` → 最终由 HBA 驱动处理，可能涉及 vendor-specific passthrough。

---

#### 2. **NVMe SSD 的健康监控（非 S.M.A.R.T.）**
NVMe 使用 **Log Page 02h (SMART/Health)**，命令不同：

```bash
# 查看 NVMe 健康状态
sudo smartctl -a /dev/nvme0n1

# 或显式指定类型
sudo smartctl -d nvme --health /dev/nvme0n1
```

**关键字段**（来自 `struct nvme_smart_log`）：
- `Critical Warning`：位掩码（温度、可用空间、可靠性等告警）
- `Available Spare`：<10% 表示寿命将尽
- `Media and Data Integrity Errors`：不可恢复错误计数

> 🔍 **内核实现**：`nvme_get_log()` → `nvme_submit_sync_cmd()` → Admin Queue

---

#### 3. **触发和监控自检（Self-test）**
专家需能主动触发测试并解析结果：

```bash
# 启动短自检（约2分钟）
sudo smartctl -t short /dev/sda

# 启动长自检（数小时）
sudo smartctl -t long /dev/sda

# 查看自检历史
sudo smartctl -l selftest /dev/sda
```

**输出解读**：
```
Num  Test_Description    Status                  Remaining  Life%  LBA_of_first_error
# 1  Short offline       Completed without error       00%      99%    -
# 2  Extended offline    Aborted by host               90%      99%    -
```

> ⚠️ **注意**：自检期间 I/O 性能可能下降；某些企业盘支持后台自检（Background Self-test）。

---

#### 4. **读取原始日志（Raw Log Pages）**
用于深度调试或解析厂商私有数据：

```bash
# 读取 ATA Log Page 0x00 (Directory)
sudo smartctl -l scttemp /dev/sda        # 温度历史（SCT）
sudo smartctl --log=0x10 /dev/sda        # 读取 Log Page 0x10（厂商定义）

# 对 NVMe 读取特定 log
sudo nvme get-log /dev/nvme0 --log-id=0x02 --len=512
```

> 🧠 **内核知识**：这对应 ATA 的 `READ LOG EXT` 命令或 NVMe 的 `Get Log Page` Admin Command。

---

### 三、调试与故障排查技巧（专家必备）

#### 1. **启用详细输出与调试模式**
```bash
# 显示命令执行过程（含寄存器值）
sudo smartctl -r ioctl,2 -a /dev/sda

# 输出十六进制原始数据
sudo smartctl -x /dev/sda
```

#### 2. **判断命令是否被内核/驱动拦截**
- 若返回 `SMART Not Supported`，但设备实际支持：
    - 检查 `dmesg | grep -i ata` 是否有 EH（Error Handling）事件
    - 确认控制器是否禁用了 SMART（如某些 USB 桥接芯片）

#### 3. **在容器或虚拟机中使用**
- **Docker**：需 `--privileged` 或挂载 `/dev` + `CAP_SYS_RAWIO`
- **QEMU/KVM**：需启用 `discard=unmap` 和 `iothread`，且 virtio-blk **不透传 SMART**（需用 IDE/SCSI 模拟）

---

### 四、安全与权限模型

- **权限要求**：`CAP_SYS_RAWIO`（通常需 root）
- **SELinux/AppArmor**：可能阻止 raw I/O，需调整策略
- **Lockdown Mode**（Secure Boot）：在 `integrity` 模式下禁止 `HDIO_DRIVE_CMD`

---

### 五、自动化与监控集成

专家应能编写脚本实现：
```bash
#!/bin/bash
# 检查健康状态并告警
if ! smartctl -q silent -H /dev/sda; then
    logger "CRITICAL: Disk /dev/sda SMART health check FAILED!"
    # 触发备份或替换流程
fi

# 监控关键属性变化
smartctl -A /dev/sda | awk '$1 == "197" && $10 > 0 { exit 1 }'
```

结合 `cron`、`Prometheus`（通过 `node_exporter` 的 textfile collector）实现长期监控。

---

### 六、常见陷阱与内核限制

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| `smartctl: Read SMART Self-test Log Failed` | 自检日志被固件清除 | 忽略或检查是否有 recent errors |
| USB 磁盘无法获取 SMART | 桥接芯片不支持 passthrough | 换芯片（如 JMS567）或直接接 SATA |
| 虚拟机中无 SMART 数据 | Hypervisor 未透传 | 使用 PCIe passthrough 或 SCSI controller |
| `HDIO_DRIVE_CMD` 返回 EPERM | Lockdown mode 启用 | 临时禁用 lockdown 或使用 signed kernel module |

---

### 总结：Linux 内核专家对 `smartctl` 的掌握标准

✅ 能解释从 `smartctl -H` 到硬盘固件的完整数据通路（用户空间 → VFS → block → SCSI → libata → AHCI → PHY）  
✅ 能在 RAID/HBA/USB/NVMe 等复杂环境中正确选择设备和 `-d` 参数  
✅ 能通过 `-r ioctl,2` 和 `dmesg` 联合调试命令失败原因  
✅ 能区分 ATA S.M.A.R.T. 与 NVMe Health Log 的实现差异  
✅ 能设计内核模块或 eBPF 程序安全地采集健康数据（避免频繁 ioctl）

> 📚 **推荐源码阅读**：
> - `smartmontools` 源码中的 `atacmds.cpp`, `scsicmds.cpp`
> - Linux 内核 `drivers/ata/libata-scsi.c` 中的 `ata_cmd_ioctl` 实现

这才是真正的“专家级”掌握。