# devres：设备资源托管（Linux 6.6）

> 阅读目标：吃透"probe 失败回滚地狱"的解药——devres（device resource management，Tejun Heo 2006，drivers/base/devres.c 1228 行）：三结构两把锁的极简地基（node 链表 + data 内嵌 + group 双 marker）、FIFO 入队/**LIFO 释放**的栈语义、group 的**两遍染色**算法（"wholly contained" 注释原话）、`devm_kmalloc` 的 **noop release** 设计（kfree 容器即释放数据）、`devm_action_*` 泛型族，以及三消费方案例（firmware 的 magic 匹配回扣 ch04 / IRQ / pcim）。全部对照 v6.6。前置：[02-总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)（really_probe 的失败路径）｜[04-firmware 加载](04-firmware加载与request_firmware生命周期.md)（fw_name_devm 的上游）。

---

## 一、问题：probe 的回滚地狱

```
传统 probe（无 devres）：
  if (!res1 = request_irq(...))      goto err0;
  if (!(res2 = ioremap(...)))        { release_irq(); goto err0; }
  if (!(res3 = clk_get(...)))        { iounmap(); release_irq(); goto err0; }
  ...每加一个资源，前面所有错误路径各加一行——O(n²) 的样板

devres 的契约（2006 年设计）：
  res = devm_XXX_alloc(dev, ...)   ← 挂到设备
  失败 return -ENOMEM 即可——内核在正确的时机按逆序释放一切：
  ① probe 失败 → device_unbind_cleanup（dd.c:547）
     → devres_release_all（:549）
  ② 驱动解绑（remove/detach）→ 同一入口
★资源生命周期绑定到 device 而非驱动代码——"谁拥有，谁释放"变成
  "挂在谁身上，谁被解绑时释放"
```

## 二、三结构两把锁（devres.c:19-43）

```c
struct devres_node {             /* :19-24 —— 链表节点 + 元信息 */
	struct list_head entry;
	dr_release_t release;         /* 释放回调（兼作类型标记，见 §七）*/
	const char *name; size_t size;
};

struct devres {                  /* :26-36 —— node + 内嵌数据 */
	struct devres_node node;
	u8 __aligned(ARCH_DMA_MINALIGN) data[];
	/* :29-34 注释原话："Some archs want to perform DMA into kmalloc
	   caches and need a guaranteed alignment larger than the alignment
	   of a 64-bit integer." */
};

struct devres_group {            /* :38-43 —— 开/闭双 marker */
	struct devres_node node[2];
	void *id; int color;
};
```

挂载点：`dev->devres_head`（list）+ `dev->devres_lock`（spinlock）——每个 device 自带账本。`add_dr`（:132）`list_add_tail` **FIFO 入队**；`find_dr`（:254）`list_for_each_entry_reverse` **从尾找**（后挂者优先——同名资源最新者胜）。

尺寸工程（check_dr_size :97-108）：`check_add_overflow` 防近 SIZE_MAX 溢出 + `kmalloc_size_roundup`——**直接分配整个 kmalloc 桶**（"Actually allocate the full kmalloc bucket size"），浪费几个字节换取零重分配。

## 三、API 阶梯

```
两段式（供新 devm_* 封装者）：
  __devres_alloc_node(:162) → alloc_dr + set_node_dbginfo（未挂设备）
  devres_add(:243)          → 挂链（此后归设备管）
  devres_free(:223)         → add 之前反悔的释放

五件套（按"匹配键"操作）：
  devres_find(:286)    找而不取（match 回调 + 匹配数据）
  devres_get(:316)     找到或挂入 new_res（惰性注册模式）
  devres_remove(:352)  摘链不释放（拿回所有权）
  devres_destroy(:390) 摘链并释放
  devres_release(:420) 匹配则触发 release（不摘链也释放）
遍历：devres_for_each_res(:190) —— 固件 suspend 缓存用它扫全部名字
```

## 四、释放：LIFO 与两遍染色

### 4.1 release_nodes（:496-512）——逆序释放

```c
list_for_each_entry_safe_reverse(dr, tmp, todo, node.entry) {
	devres_log(dev, &dr->node, "REL");   /* tracepoint + DEBUG_DEVRES */
	dr->node.release(dev, dr->data);     /* ★释放回调只管资源本体 */
	kfree(dr);                           /* 容器（连同 data）在此释放 */
}
```

★**这就是 devm_kmalloc 的 release 是 noop 的原因**（:798-801）——内存就是 data 本身，`kfree(dr)` 一步到位；普通资源（IRQ/映射）的 release 才有实事可做。

### 4.2 remove_nodes 的两遍染色（:435-493）

要把**一段范围**（而非全链）移入 todo——group 边界可能被切断。算法（注释原话 :467-472）：*"A group gets color value of two iff the group is wholly contained in [current node, end). That is, for a closed group, both opening and closing markers should be in the range, while just the opening marker is enough for an open group."*

```
第一遍：普通节点移入 todo；group marker 清零 color
第二遍：扫 marker——每个 group 的 open/close 计数；color==2（整组在范围内）
        → open marker 入 todo、close marker 删除（组员已在 todo）
BUG_ON(color<=0 || color>2) —— 结构不变式的自证
```

`devres_release_all`（:517）就是全范围版（first=链头、end=链尾）——`device_unbind_cleanup`（dd.c:547-549）在 **really_probe 失败**（:698/:724 两处 goto）与解绑时调用。

## 五、devm 双家族

```
内存族：devm_kmalloc(:821, EXPORT_SYMBOL_GPL :841)
  devm_kmalloc_release = noop（§4.1）—— data 即资源
  ZERO_SIZE_PTR 特例（:830）

action 族（现代推荐的泛型口，:703-796）：
  struct action_devres { void (*action)(void *); void *data; void *name; }
  devm_add_action(dev, action, data)      ← 任意函数 + 上下文
  devm_remove_action / devm_release_action  ← 按 (action, data) 双元组
                                                match / 主动提前触发
  ★新驱动不再需要手写 struct+match+release 三件套封装——
    一个回调直接挂账（devres.rst 镜像中 152 处 devm_ 的"逃生舱"）
```

## 六、消费方三案例（已核对）

| 案例 | 机制 | 出处 |
| :--- | :--- | :--- |
| **firmware** | `struct fw_name_devm {magic, name}`——**magic = `(unsigned long)&fw_cache`**（实例魔数，防伪造）；`fw_find_devm_name` 用 devres_find+fw_devm_match 判"该设备是否已为 suspend 缓存登记此名"；suspend 侧 `devres_for_each_res` 扫全表（fw main.c:639/:1412） | [04 章](04-firmware加载与request_firmware生命周期.md) 的登记簿底层 |
| **IRQ** | `struct irq_devres {irq, dev_id}` + `devm_irq_match` 按 **(irq, dev_id) 双元组**匹配（free 与 request 的共享键）——devm_request_threaded_irq（kernel/irq/devres.c:51） | 注释提醒：手动 free 用 devm_free_irq |
| **PCI** | `pcim_release`——pcim_* 全家的总释放器（pci.c:2128/:2141 用 devres_find 反查"是否已接管"） | pci_ioremap/pcim_enable 的地基 |

## 七、group：probe 的回滚窗口

```
devres_open_group(dev, id, gfp)     (:552) 插入 open marker（noop release）
  ...此后的 devm_* 资源都落在开区间内...
devres_close_group(dev, id)         (:609) 插入 close marker
devres_release_group(dev, id)       (:668) 把整组资源提前释放——
                                       "probe 中途想重开一局"的窗口
```

**类型标记技巧**：group marker 的 release 是 noop（:78-85），但 `node_to_group`（:88-95）**用 release 指针本身**（`== &group_open_release`）判类型——零额外字段。树内用户：libata（libata-core.c:5579/:5611）与 libahci_platform（:466/:662）——ATA 主机探测的多段回滚。

## 八、观测实战

```bash
# 1) devres 生命周期全录像（CONFIG_DEBUG_DEVRES=y）
echo 1 > /sys/module/devres/parameters/log     # module_param_named(log...)
# dmesg 形如：DEVRES ADD <ptr> devm_kmalloc_release (32 bytes)
#             DEVRES REL <ptr> devm_kmalloc_release (32 bytes)

# 2) tracepoint（无需 DEBUG_DEVRES）
perf record -e devres:devres_log -a sleep 5 && perf script
#   drivers/base/trace.h:41-42 定义（dev/op/node/name/size 五字段）

# 3) 泄漏排查：unbind 后 dmesg 仍见该设备的 REL？
#    ——设备没真正解绑（driver 还挂着：ls -l /sys/bus/.../drivers/.../）

# 4) 家族全表：Documentation/driver-api/driver-model/devres.rst 镜像
#    （152 处 devm_* 的分类索引——本仓库 rst 镜像直接可查）
```

## 九、思考题

1. `find_dr` 用 `_reverse`（从链尾找）——同一个 (release, match) 键挂了两条会命中谁？这在什么封装模式下是特性？（提示：devres_get 的惰性注册与 devm_krealloc 的"新替旧"。）
2. `devm_kmalloc` 的 release 是 noop、`devres_destroy` 却仍能单独释放它——kfree(dr) 发生在哪一层的哪个函数？（对照 §4.1 的两层：回调管资源 vs release_nodes 管容器。）
3. group 的第二遍染色里"close marker 被单独 list_del_init 而不入 todo"——为什么 open 入 todo、close 直接删？（提示：todo 的释放方向、marker 自身无需 release。）
4. firmware 的 magic 用 `(unsigned long)&fw_cache` 而非常量——防什么？（提示：谁可能往同一 devres 链挂同名结构？跨模块伪造的成本。）
5. probe 失败路径 device_unbind_cleanup 在 :698 与 :724 两处调用——分别对应 really_probe 的哪个失败段？（提示：一处在 driver 的 probe 返回错误后，另一处在……对照 [02 章](02-总线-设备-驱动匹配.md)的 really_probe 全链。）
6. devm_action 与"手写 devm 封装"的取舍：什么资源仍然必须手写（match 语义太复杂）？（提示：双元组匹配够不够——按 irq+dev_id 找 vs 按回调指针找。）

---

> 回看：[02-总线-设备-驱动匹配](02-总线-设备-驱动匹配.md)（device_unbind_cleanup 的宿主路径）｜[04-firmware 加载](04-firmware加载与request_firmware生命周期.md)（fw_name_devm 的消费端）｜[01-kobject 与 sysfs](01-kobject与sysfs.md)（dev->devres_head 所在的 device 结构）｜[interrupt 分册](../../interrupt/README.md)（devm_request_threaded_irq 的 IRQ 语义）｜[trace/05-tracer 家族](../../trace/docs/05-tracer家族.md)（devres_log tracepoint 的消费）
