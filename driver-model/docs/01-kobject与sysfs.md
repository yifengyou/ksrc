# kobject 与 sysfs（Linux 6.6）

> 阅读目标：掌握 kobject/kset/kobj_type 三件套的职责边界与生命周期（kref 引用计数 + release 协议）；吃透 sysfs 的 kernfs 地基（cgroup 分册已铺路）与 uevent 热插拔广播机制；理解 device 的 attribute 家族（普通/二进制/group）与内核侧创建目录树的规则。

---

## 一、三件套（lib/kobject.c + include/linux/kobject.h，已核对 6.6 字段）

```c
struct kobject {
	const char		*name;         /* 目录名（可改：改名=移目录）*/
	struct list_head	entry;         /* 挂入 kset 的链 */
	struct kobject		*parent;       /* 父目录（目录树的边）*/
	struct kset		*kset;         /* 所属集合（提供默认 ktype/subsystem）*/
	const struct kobj_type	*ktype;  /* ★release/默认属性组的宿主 */
	struct kernfs_node	*sd;       /* sysfs 目录节点（kernfs 柄）*/
	struct kref		kref;          /* ★引用计数（全机制的命脉）*/
	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;   /* 目录已建 */
	unsigned int state_add_uevent_sent:1;
	unsigned int state_remove_uevent_sent:1;
	unsigned int uevent_suppress:1;
};

struct kset { /* kobject 的容器 + 事件链：list（成员）、kobj（自身目录）、
                uevent_ops（过滤/环境变量注入）——既是目录又是子系统锚点 */ }
struct kobj_type { void (*release)(struct kobject *); /* ★归零时的析构：
                    kobject 是嵌入结构——宿主对象才知道怎么 free */
                   const struct sysfs_ops *sysfs_ops;
                   const struct attribute_group **default_groups; }
```

**核心契约**：`kobject` 自己不拥有内存——它总是嵌在 `device`/`module` 等宿主里。`kref` 归零只调 `ktype->release`，由宿主的 release 决定真正的 `kfree`。忘掉 release（ktype 为 NULL）会 WARN 并泄漏——这是新驱动最常见的一类。

## 二、sysfs：kernfs 上的对象树

sysfs（`/sys`）的每个目录=一个 kobject，每个文件=一个 attribute。地基 kernfs 与 cgroupfs **同一引擎**（cgroup 分册第 1 章）——差异只在：sysfs 挂载在根 kset 树上、由内核对象层（`fs/sysfs`）包装 attribute 读写协议：

```
读 sysfs 文件 → kernfs seq_read → kobj->ktype->sysfs_ops->show(attr, buf)
写 sysfs 文件 → ...->store(attr, buf, count)
宿主用宏落地：DEVICE_ATTR(_RO/_WO) 生成 device_attribute{attr, show, store}
             sysfs_create_group 按表批量建文件
```

规则速记：一个 kobject 一个目录；`kernel_kset`/`devices_kset` 等根锚由 kobject_create_and_add 建立；**属性必须用 `*_show/*_store` 协议**（不许 mmap/seek 的普通文件）。

## 三、uevent：热插拔的内核→用户态广播

```
kobject_uevent(kobj, KOBJ_ADD/REMOVE/CHANGE)
 ├─ 过滤：kset->uevent_ops->filter
 ├─ 环境拼装：uevent_ops->uevent + 子系统自定义（DEVNAME/DEVPATH/MODALIAS...）
 └─ 两条投递通道：
     ① netlink KOBJ_EVENT 组（udev/mdev 走这条——冷启动后的事件流）
     ② uevent_helper（/sys/kernel/uevent_helper，默认空——现代发行版已弃用）
冷启动引导（先有鸡问题）：设备早于 udev 存在——device_init 扫 /sys 的 modalias
                          触发 modprobe（module 分册的 request_module 链）
```

`MODALIAS` 是驱动绑定的请求信封（pci:vXXXdYYY... / of:NxyzT...）——udev 据此 `modprobe $MODALIAS`，与第二章的自动绑定形成闭环。

## 四、观测与实战

```
tree /sys | less                        # 目录树=对象图
udevadm monitor --kernel --udev         # 双侧事件流对照（时间差=用户态处理延迟）
udevadm info -a /dev/sdb                # 沿 kobject 链上溯的属性全景
echo add > /sys/.../uevent              # 手动重放事件（调 udev 规则）
ls /sys/kernel/uevent_seqnum            # 全局事件号（乱序排查）
```

## 五、思考题

1. `kobject_put` 与 `kref` 的 release 协议为什么要经 ktype 间接？（对照"宿主才知道怎么 free"与嵌入约定。）
2. sysfs 与 cgroupfs 共用 kernfs——两者的挂载语义差异在哪？（提示：结构树的建立者：内核对象层 vs cgroup 框架。）
3. uevent 的 netlink 与 `uevent_helper` 两条通道的时序保证各是什么？为什么冷启动必须依赖 modalias 扫描而非事件流？
4. 设备目录改名（`kobject_rename`）在 uevent 侧需要哪些联动？（DEVNAME 与 symlink 的一致性。）
5. 设计：一个新总线类型需要哪些 kobject/kset 脚手架？（对照 `bus_register` 的产物。）

---

> 下一章：[总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)。
