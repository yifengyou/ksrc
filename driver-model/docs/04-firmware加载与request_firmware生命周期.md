# firmware 加载与 request_firmware 生命周期（Linux 6.6）

> 阅读目标：吃透固件从 `request_firmware()` 调用到 `release_firmware()` 的完整生命周期——五级搜索路径的次序与理由、builtin/缓存/在途请求的三级快路径、内核凭证切换的安全设计（`prepare_kernel_cred`）、sysfs fallback 的 `loading`/`data` 协议与 uevent 联动、suspend 期的固件缓存（PM notifier 驱动）、以及 fw_upload 的用户态刷新通道。所有调用链与报错消息逐行核对自 v6.6（drivers/base/firmware_loader/{main.c,sysfs.c,fallback.c,fallback_table.c}、include/linux/firmware.h）。前置：[01-kobject 与 sysfs](01-kobject与sysfs.md)（uevent 机制在本章 fallback 中被复用）。

---

## 一、概览：一个"从内核到用户态再回来"的补给线

```
驱动 probe:
  request_firmware(&fw, "brcm/bcm43xx-0.fw", dev)
      │
      ▼
  _request_firmware (main.c:849)
      │
      ├─ ① builtin？(CONFIG_EXTRA_FIRMWARE 编译进 vmlinux)
      ├─ ② 缓存/在途请求？(fw_cache 哈希命中 → 共享 kref)
      ├─ ③ 内核凭证切换 → 直读文件系统（五级路径）
      ├─ ④ .zst / .xz 解压回退（按配置）
      ├─ ⑤ platform 查询（EFI runtime 等）
      └─ ⑥ sysfs fallback：uevent → udev/用户态写 /sys/class/firmware/*/data
      │
      ▼
  assign_fw → 驱动拿到 fw->data/fw->size
      │
  驱动烧录设备 ──► release_firmware(fw)
```

**目录演变对源注**：老内核的 `drivers/base/firmware_class/` 在 5.x 已改名 `drivers/base/firmware_loader/`（本仓 v6.6 实证），但**模块参数名仍叫 `firmware_class.path=`**（main.c:480 注释原样："firmware_class is generally built in kernel instead of module"）——历史包袱的活化石。

## 二、公开 API 家族与 struct firmware（include/linux/firmware.h）

公开结构体刻意做小——所有加载器私有状态藏在 `priv`：

```c
struct firmware {
	size_t size;
	const u8 *data;
	void *priv;         /* firmware loader private fields */
};
```

| API（导出处） | 语义 | opt_flags（main.c 已核对） |
| :--- | :--- | :--- |
| `request_firmware` (:951, EXPORT_SYMBOL) | 同步阻塞加载 | `FW_OPT_UEVENT` |
| `request_firmware_direct` (:1003, EXPORT_SYMBOL_GPL) | **不走 fallback**，探针路径友好 | `FW_OPT_UEVENT \| FW_OPT_NO_WARN \| FW_OPT_NOFALLBACK_SYSFS` |
| `firmware_request_platform` | 允许 EFI platform 查询 | — |
| `request_firmware_into_buf` (:1081) | 装入调用者预分配缓冲（DMA 场景） | `FW_OPT_UEVENT \| FW_OPT_NOCACHE` |
| `request_partial_firmware_into_buf` (:1107) | 支持 `offset` 局部读取 | 上者 + `FW_OPT_PARTIAL` |
| `request_firmware_nowait` (:1194) | 异步：workqueue + 回调 `cont` | `FW_OPT_NOWAIT \| (uevent ? FW_OPT_UEVENT : FW_OPT_USERHELPER)` |
| `firmware_request_cache` (:1053, EXPORT_SYMBOL_GPL) | 手动登记 devm 缓存名 | — |
| `release_firmware` (:1127) | 释放（builtin 的 data 不释放） | — |

★ `FW_OPT_PARTIAL` 的代价（:886 注释原样 *"Only full reads can support decompression, platform, and sysfs."*）：局部读**跳过**解压、platform、sysfs 三条回退路径——预分配缓冲的模式天然假定一次到位。

## 三、核心加载链 _request_firmware（main.c:849-928）

### 3.1 快路径：_request_firmware_prepare（:737）

```c
if (firmware_request_builtin_buf(firmware, name, dbuf, size)) {
	dev_dbg(device, "using built-in %s\n", name);
	return 0; /* assigned */
}
ret = alloc_lookup_fw_priv(name, &fw_cache, &fw_priv, dbuf, size, ...);
if (ret > 0) {              /* 同名请求在途（in-flight） */
	ret = fw_state_wait(fw_priv);   /* 睡眠等待对方完成 */
	if (!ret) {
		fw_set_page_data(fw_priv, firmware);
		return 0; /* assigned */
	}
}
return 1;  /* need to load */
```

三级快路径：**builtin**（`CONFIG_EXTRA_FIRMWARE` 编译进内核映像；firmware.h:81-84 注释：仅 `FW_LOADER=y` 可用、`=m` 不行）→ **缓存命中** → **在途共享**（同名请求挂 `fw_state_wait` 等待，多个 waiter 共享同一 `fw_priv` 的 kref——批量请求"只需一次唤醒"（:780 注释原话：*"Batched requests need only one wake"*））。

### 3.2 内核凭证切换：为什么直读文件要 override_creds

```c
/* main.c:872-876 注释原样：
 * "Because we may have been called by a driver when serving an
 *  unrelated request from userland, we use the kernel credentials
 *  to read the file."
 */
kern_cred = prepare_kernel_cred(&init_task);
old_cred = override_creds(kern_cred);
... /* 文件读取主体 */
revert_creds(old_cred);
```

驱动可能在意外的进程上下文里被调用（如 sysfs 写触发 probe），**读固件必须用 init_task 的凭证**——否则文件系统权限检查会看到"任意调用者"的 uid，既有越权读风险也可能被无关的 LSM 策略误伤。读文件本身走 `kernel_read_file_from_path_initns`（:548，`READING_FIRMWARE` 标识；:547 注释：*"load firmware files from the mount namespace of init"*——用 init 的 mount namespace，不受驱动调用者的 nsview 影响）。

### 3.3 五级搜索路径（fw_path[]，:471-477 逐字）

```c
static char fw_path_para[256];
static const char * const fw_path[] = {
	fw_path_para,                          /* firmware_class.path= 自定义 */
	"/lib/firmware/updates/" UTS_RELEASE,  /* 内核版本专属更新 */
	"/lib/firmware/updates",               /* 通用更新 */
	"/lib/firmware/" UTS_RELEASE,          /* 内核版本专属 */
	"/lib/firmware"                        /* 标准位置 */
};
```

次序即策略：**更新优先于基线**、**版本专属优先于通用**——发行版可以不动 `/lib/firmware` 只往 updates 放补丁。`fw_path_para` 由 `module_param_string(path, ..., 0644)`（:484）注册，cmdline `firmware_class.path=/opt/fw` 一路直达最高优先级。

### 3.4 回退链的完整次序（:884-910）

```
fw_get_filesystem_firmware(device, fw->priv, "", NULL)   原始文件
  └ -ENOENT 且非 PARTIAL:
     .zst 变体   (CONFIG_FW_LOADER_COMPRESS_ZSTD)
     .xz 变体    (CONFIG_FW_LOADER_COMPRESS_XZ)
     firmware_fallback_platform()   EFI/平台数据
  └ 仍失败:
     "Direct firmware load for %s failed with error %d"  ← ★真实报错（:906）
     firmware_fallback_sysfs(fw, name, device, opt_flags, ret)
  └ 成功:
     assign_fw(fw, device)
```

压缩固件语义（Kconfig:182-183 原样）：*"The compressed file is loaded as a fallback, only after loading the raw file failed at first."*——`name` 不带后缀，加载器自己试 `.zst/.xz`；XZ 文件必须用 `xz -C crc32` 压制（Kconfig 明文）。

### 3.5 assign_fw 的三件事（:690）

1. 空文件/被中止 → `-ENOENT`（拒绝空固件：`!fw_priv->size || fw_state_is_aborted`）
2. `FW_OPT_UEVENT && !FW_OPT_NOCACHE` 且有 device → `fw_add_devm_name`：名字进 devres 链表——**suspend 缓存与设备卸载自动 uncache 的登记簿**
3. 全局处于 `FW_LOADER_START_CACHE` → `fw_cache_piggyback_on_request`：正赶上 suspend 前缓存窗口，捎带登记
4. 最后 `fw_set_page_data` 把页缓冲交给驱动（"at the last minute"，:726 注释——多 waiter 同时拿到映射）

## 四、fw_priv 与状态机（firmware_loader/firmware.h）

公开 `struct firmware` 背后的真身：

```c
struct fw_priv {
	struct kref ref;              /* 多请求共享计数 */
	struct list_head list;        /* fw_cache 哈希链 */
	struct fw_state fw_st;        /* 完成通知状态机 */
	void *data;
	size_t size, allocated_size, offset;
	u32 opt_flags;
#ifdef CONFIG_FW_LOADER_PAGED_BUF
	bool is_paged_buf;
	struct page **pages; int nr_pages, page_array_size;  /* sysfs fallback 的页池 */
#endif
#ifdef CONFIG_FW_LOADER_USER_HELPER
	bool need_uevent;
	struct list_head pending_list;   /* kill_pending_fw_fallback_reqs 的清单 */
#endif
	const char *fw_name;
};
```

状态机四态（:47-52）：`FW_STATUS_UNKNOWN → LOADING → DONE`，任一环节可 `ABORTED`；等待方 `fw_state_wait` 睡在 `__fw_state_wait_common`（:98）上，`-ERESTARTSYS` 被换算为 `-EINTR` 返回（fallback.c:114-115 已核对）。

## 五、异步 request_firmware_nowait（:1143-1218）

```c
struct firmware_work {
	struct work_struct work;
	struct module *module;        /* try_module_get 防模块先走 */
	const char *name;
	struct device *device;        /* get_device 防设备先走 */
	void *context;
	void (*cont)(const struct firmware *fw, void *context);
	u32 opt_flags;
};

static void request_firmware_work_func(struct work_struct *work)  /* :1153 */
{
	_request_firmware(&fw, ...);
	fw_work->cont(fw, fw_work->context);   /* fw 可能为 NULL（失败） */
	put_device(fw_work->device);
	module_put(fw_work->module);
}
```

- 普通路径 `schedule_work`——**system_wq**，probe 里 `GFP_KERNEL` 场景注释明示"sleep for as small periods as possible since it may increase kernel boot time"（:1186-1191）；`GFP_ATOMIC` 变体完全不能睡
- `uevent=false` 时走 `FW_OPT_USERHELPER`（无 uevent 的纯 sysfs 手动模式），且若 `fw_cache_is_setup` 直接 `-EOPNOTSUPP`（:1217——缓存机制假定 uevent 模式）
- 回调 `cont` 里失败时 `fw == NULL`——**回调必须判空**

## 六、suspend 固件缓存（CONFIG_FW_CACHE，main.c:1236-1610）

问题：resume 后设备重新初始化需要固件，但此时用户态（rootfs/udev）未必就绪——**固件必须提前钉在内核内存里**。

```
PM_SUSPEND_PREPARE / PM_HIBERNATION_PREPARE / PM_RESTORE_PREPARE
  └─ fw_pm_notify (:1519)
       ├─ kill_pending_fw_fallback_reqs(true)   /* 杀掉在途 fallback，防拖死挂起 */
       └─ device_cache_fw_images (:1466)
            ├─ fw_fallback_set_cache_timeout → 10 秒   /* fallback.c:19-22 注释：
            │    "about 2M bytes, so 10 secs should be enough" */
            ├─ fwc->state = FW_LOADER_START_CACHE
            ├─ dpm_for_each_dev(NULL, dev_cache_fw_image)  /* 遍历 devres 登记簿 */
            └─ async_synchronize_full_domain(&fw_cache_domain)

PM_POST_SUSPEND / PM_POST_HIBERNATION / PM_POST_RESTORE
  └─ fw_cache.state = FW_LOADER_NO_CACHE
  └─ device_uncache_fw_images_delay(10 * MSEC_PER_SEC)  /* 延迟 10s 释放 */
```

细节三则：① syscore 阶段由 `fw_syscore_ops`（:1552-1559）置 `FW_LOADER_NO_CACHE` 兜底（*"stop caching firmware once syscore_suspend is reached"*）；② 缓存窗口内新到的请求会被 `assign_fw` 捎带登记（piggyback，第三节）；③ Kconfig 提醒该机制"generates uevent messages that are sent over a netlink socket, it can prevent suspend on many platforms"（:212-214）——uevent 风暴是它默认 y 但可关的原因。

## 七、sysfs fallback：loading/data 协议（CONFIG_FW_LOADER_USER_HELPER）

### 7.1 设备与 uevent

```c
static struct class firmware_class = {          /* sysfs.c:102 */
	.name		= "firmware",
	.class_groups	= firmware_class_groups,   /* timeout 类属性 */
	.dev_uevent	= firmware_uevent,
	.dev_release	= fw_dev_release,
};
```

`fw_load_sysfs_fallback`（fallback.c:70）：创建 `struct device`（名字即固件名）→ **先抑制 uevent** `device_add` → 挂入 `pending_fw_head` → 放开抑制、`kobject_uevent(KOBJ_ADD)` 广播。uevent 环境变量（sysfs.c:69-73）：

```
FIRMWARE=brcm/bcm43xx-0.fw
TIMEOUT=60
ASYNC=0
```

udev 收到后按 `/sys/class/firmware/<name>/{loading,data}` 协议回灌固件。

### 7.2 loading 的三值协议（sysfs.c:141-229 注释+代码）

```
echo 1  > loading   开始：丢弃历史部分数据，fw_state_start
echo 0  > loading   完成：fw_map_paged_buf 映射页池
                      → security_kernel_post_load_data(data, size,
                          LOADING_FIRMWARE, "blob")   ★LSM 钩子
                      → fw_state_done（唤醒所有等待者）
                      → fw_upload_start（若是 fw_upload 通道）
echo -1 > loading   失败中止：fw_load_abort
```

★ `security_kernel_post_load_data` 是 lockdown/IMA 的介入点——**从用户态灌进来的 blob 必须过安全检查**（对比：文件系统直读走 `kernel_read_file_from_path_initns` 的 `READING_FIRMWARE` 路径，在 LSM 侧另有 `security_kernel_read_file` 检查）。`data` 文件按页写入 `fw_priv->pages` 页池（`CONFIG_FW_LOADER_PAGED_BUF`），完成后 `fw_map_paged_buf` 一次性 vmap。

### 7.3 超时与开关

- 默认 60 秒（fallback_table.c:22：`loading_timeout = 60`）；`/sys/class/firmware/timeout` 可调（秒）
- 超时换算：`__firmware_loading_timeout() * HZ`，值 `<= 0` 时映射 `MAX_JIFFY_OFFSET`（无限等，fallback.c:36-40）
- `/proc/sys/kernel/firmware_config/{force_sysfs_fallback,ignore_sysfs_fallback}`（fallback_table.c:28-54）——前者等价 `CONFIG_FW_LOADER_USER_HELPER_FALLBACK`（强制所有非 direct 请求走 fallback），后者内核侧忽略 uevent

## 八、fw_upload：反向通道（CONFIG_FW_UPLOAD，sysfs_upload.c）

6.6 的固件上传 API 与 fallback 共用 loading/data 协议，方向相反：**用户把镜像灌给驱动去烧写设备硬件**（FPGA 卡刷 FLASH 的典型场景，Kconfig:226 起原文："FPGA based PCIe cards load firmware and FPGA images from local FLASH when the card boots"）。

```
/sys/class/firmware/<name>/
  data      同一写入协议（echo 0 > loading 后触发 fw_upload_start）
  status    只读：fw_upload_prog 状态机字符串
  remaining_size  只读：剩余待烧写字节（sysfs_upload.c:112-119）
  cancel    只写：ops->cancel
驱动侧：firmware_upload_register(module, parent, name, &fw_upload_ops, priv)
  （sysfs_upload.c:291，EXPORT_SYMBOL_GPL :379）
```

状态机（`enum fw_upload_prog`）以 `fw_upload_prog_str[]` 字符串化；错误码族 `FW_UPLOAD_ERR_*`（firmware.h:enum——HW_ERROR/TIMEOUT/CANCELED/BUSY/INVALID_SIZE/RW_ERROR/WEAROUT）——WEAROUT（FLASH 接近写穿，等待重试）是唯一的"可等待重试"错误。

## 九、观测与实战

```
失败第一现场（dmesg）：
  "Direct firmware load for %s failed with error %d"   ← 直读五级全空
  "firmware: %s loading timed out"                      ← NOWAIT 撞 usermodehelper 锁（fallback.c:138，dev_dbg 级）
  "loading %s failed with error %d"                     ← 某路径存在但读取失败（:554，dev_warn）

定位清单：
  ① ls /lib/firmware/updates/$(uname -r)/  检查五级路径哪级有货
  ② 确认压缩变体：.zst/.xz 是否被 CONFIG 打开
  ③ cat /sys/class/firmware/timeout        fallback 窗口
  ④ udev 规则/日志                          uevent 是否送达（ASYNC/FIRMWARE 变量）
  ⑤ /proc/sys/kernel/firmware_config/*     是否被强制/忽略 fallback

builtin 固件：CONFIG_EXTRA_FIRMWARE="brcm/bcm43xx-0.fw"
  + CONFIG_EXTRA_FIRMWARE_DIR（默认 /lib/firmware）——
  编译期嵌入 vmlinux，运行时零文件系统依赖（initrd 缺失场景的终极解法）
  dmesg 见 "using built-in %s"（:753，dev_dbg 级）

性能注：request_firmware 是全路径里少有的
  "内核态睡用户态"接口——直读成功时开销可忽略；
  fallback 触发时一次 uevent 往返 + sysfs 页写入，
  高频路径应避免（用 request_firmware_direct 明确放弃 fallback）
```

## 十、思考题

1. 为什么 `_request_firmware` 要 `override_creds(prepare_kernel_cred(&init_task))` 而不用 `KERNEL_DS` 之类的老办法？（提示：文件系统权限与 LSM 检查消费的是 `current_cred()`；对比 set_fs 时代已废除的方案。）
2. 五级搜索路径把 `updates/UTS_RELEASE` 放最前、`/lib/firmware` 放最后——如果发行版想做"全局紧急回滚"，应该往哪级放文件？（提示：路径不存在时返回 -ENOENT 才会落到下一级。）
3. suspend 缓存为什么要在 PM_POST_SUSPEND 后**延迟 10 秒**才 uncache，而不是立即释放？（提示：resume 后 probe 失败重试、resume_complete 与用户态就绪的时间差。）
4. `request_firmware_into_buf` 为什么天然不能进缓存（FW_OPT_NOCACHE）？（提示：缓冲区属于调用者，生命周期加载器不可控。）
5. fallback 的 `loading` 写 0 时为什么必须做 `security_kernel_post_load_data`，而 builtin 固件不需要任何 LSM 检查？（提示：信任锚——编译期签名链 vs 运行时用户态注入。）
6. `kill_pending_fw_fallback_reqs(true)` 的参数 `only_kill_custom` 为 true 时只杀 `need_uevent==false` 的请求——为什么不连 uevent 型一起杀？（提示：uevent 型有 udev 在跟；custom 型的用户态可能早已消失。）

---

> 关联：[01-kobject 与 sysfs](01-kobject与sysfs.md)（uevent/kobject_uevent 基础）｜[02-总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)（probe 上下文）｜[module/01-模块布局与加载器](../../module/docs/01-模块布局与加载器.md)（modalias→modprobe 的另一条冷启动补给线）｜[power/05-suspend 与 s2idle](../../power/docs/05-suspend与s2idle.md)（PM notifier 阶段）｜[ras/kdump/03-kexec 签名与 lockdown](../../ras/kdump/docs/03-kexec签名与lockdown.md)（security_kernel_post_load_data 的 lockdown 联动）
