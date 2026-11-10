# css_set 深挖：任务与层级的枢纽（Linux 6.6）

> 定位：[01-v2层级与线程模式](01-v2层级与线程模式.md) 的数据结构纵深——css_set 的查找/创建/释放全函数走读。前置：01 章、[process-mgmt/01-fork](../../process-mgmt/docs/01-fork与clone路径全解.md)（fork 时的 cset 复制）。

---

## 一、角色：成员关系的"组合去重"枢纽

```
任务属于哪个 cgroup（每个层级各一个）？答案是一张 subsys[] 表——
css_set。**相同组合的任务共享同一个 css_set 实例**：
  多层级 × 多任务的成员关系 = 极少量 css_set（组合数 << 任务数）
  ——内存与遍历的双重红利（fork 里 get_css_set 即引用+1）
```

## 二、结构（include/linux/cgroup-defs.h，已核对 6.6）

```c
struct css_set {
	struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT]; /* ★每子系统的 css */
	refcount_t refcount;
	struct css_set *dom_cset;      /* threaded 模式的域主（自指向=普通）*/
	struct cgroup *dfl_cgrp;       /* v2 默认层级上的 cgroup */
	int nr_tasks;
	struct list_head tasks;         /* 常驻任务链（task->cgroups 链回）*/
	struct list_head mg_tasks;      /* 迁移中任务链 */
	struct list_head dying_tasks;
	struct list_head task_iters;
	struct list_head e_cset_node[CGROUP_SUBSYS_COUNT]; /* ★每子系统的"有效
	      cset"链（挂 css->cgroup->e_csets[]）——遍历子树的加速索引 */
	struct list_head threaded_csets, threaded_csets_node; /* 域主↔成员 */
	struct hlist_node hlist;        /* 挂 css_set_table 哈希桶 */
	struct list_head cgrp_links;    /* cgrp_cset_link：与每个 root 各连一条 */
	struct list_head mg_src/dst_preload_node, mg_node;    /* 迁移机械 */
};
init_css_set：静态实例（root_cset；dom_cset 自指向）——根组合零分配
```

## 三、查找：哈希与模板（cgroup.c，已核对）

```
css_set_hash(css)：Σ subsys[i] 指针，(key>>16) ^ key
  → hash_for_each_possible(css_set_table, cset, hlist, key)
find_existing_css_set(old_cset, cgrp, template):
  构造 template：对每个子系统——
    目标 root 绑定了它（root->subsys_mask）→ cgroup_e_css_by_mask(cgrp)
    否则 → 继承 old_cset->subsys[i]     ★只换"动到的 root"，其余照抄
  compare_css_sets 全字段比对命中 → 返回既有实例（get_css_set）
```

## 四、创建：find_css_set（find-or-create 全函数，已核对）

```
持 cgroup_mutex；css_set_lock 下查哈希 → 未命中：
  ① kzalloc(cset) + allocate_cgrp_cset_links(cgroup_root_count)
     ——为每个 root 预备一条 cgrp_cset_link（连边对象）
  ② 初始化（refcount=1、dom_cset 自指向、各链 INIT）
  ③ css_set_lock 下：挂哈希桶 + css_get(每个 css) +
     e_cset_node[ssid] 挂进 css->cgroup->e_csets[ssid]（加速索引建立）
     + cgrp_links 逐 root 连边
  ④ threaded 场景：目标 dfl_cgrp 是 threaded 域 → 递归 find_css_set
     找 dom_cset（成员的域主 = 域 cgroup 的 dom_cgrp 组合），
     cset->dom_cset = dcset 并挂 threaded_csets 链
```

## 五、释放：put_css_set_locked（已核对全函数）

```
refcount_dec_and_test 归零后：
  逐子系统：e_cset_node 摘链 + css_put
  hash_del + css_set_count--
  逐 cgrp_cset_link：双向摘链 + cgroup_put（非根）+ kfree
  threaded 成员：摘 threaded_csets_node + 递归 put(dom_cset)
  kfree_rcu(cset)（迭代器可能在读——RCU 宽限期内安全）
```

## 六、迁移的挂点（呼应 01 章 attach 流程）

```
cgroup_attach_task → cgroup_migrate:
  mg_tasks 链承载"迁移中"任务（原子性窗口）
  mg_src/dst_preload_node：两阶段 preload（cgroup_migrate_prepare
  先批量分配 link/cset——避免迁移路径内存失败半途回滚）
  迁移完成 = task->cgroups 换指针 + 旧 cset put
```

## 七、观测与实战

```
cat /proc/self/cgroup          # 自己的组合（每行一个层级）
ls /sys/fs/cgroup/.../cgroup.procs 同组成员 = 同 css_set
systemd 实例大量任务同组合 → css_set 数远小于任务数（红利可观测：
  容器内 top 的 PID 数 vs 内核 css_set 计数）
调试：cgroup.procs 写入失败的 ENOSPC 多与 css_set 侧配额/内存相关
```

## 八、思考题

1. `subsys[]` 存 css（不是 cgroup）——为什么多对多的锚点是 css？（提示：每层级一套子系统状态、disabled 子系统的 NULL 槽位语义。）
2. template 构造只替换目标 root 的绑定子系统——跨 root 任务（如 v1 cpu + v2 unified 混挂）的行为如何保持不变？
3. `e_cset_node[]` 的加速索引让哪类遍历变快？（对照 css_for_each_descendant_pre 与 01 章的层级遍历。）
4. 释放路径为何用 `kfree_rcu` 而非直接 kfree？（提示：task_iters/cgroup iterators 的无锁读。）
5. 迁移的两阶段 preload 防的是什么故障模式？（推演：cgroup_migrate 中途 GFP_KERNEL 失败的回滚代价。）

---

> 回看：[01-v2层级与线程模式](01-v2层级与线程模式.md)｜[06-uclamp 深挖](06-uclamp深挖.md)｜[05-memcg 回收深挖](05-memcg回收深挖.md)
