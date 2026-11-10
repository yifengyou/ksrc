# memcg 回收深挖：从 try_charge 到 MGLRU（Linux 6.6）

> 定位：[03-memory控制器](03-memory控制器.md) 的执行路径纵深——回答"超过 memory.max/high 之后内核到底做了什么"。前置：03 章（限额模型）、mm 分册 [pagecache 与 writeback](../../mm/pagecache/pagecache与writeback.md)（脏页回写语义）。

---

## 一、回收的三种触发源

```
① charge 限流：try_charge 途中发现越限 → 主动回收/惩罚/OM（下节）
② 全局水位：kswapd 与直接回收（mm 分册 buddy 章的 waterline）——
   memcg 无关但共享同一台回收机器
③ 用户显式：echo 256M > memory.reclaim（memcontrol.c:6689 已核对，
   强制该 cgroup 回收指定量——6.6 现役接口）
```

## 二、try_charge 全链（mm/memcontrol.c，已核对 6.6）

```
folio 计费 → try_charge(memcg, gfp, nr_pages):
  ① per-CPU stock 先扣（MEMCG_CHARGE_BATCH 批量预充——摊薄原子操作）
  ② page_counter_try_charge：memory 与 memsw（若计 swap）双计数
  ③ memory.high 越限 → memcg_high_try_to_reclaim：
     try_to_free_mem_cgroup_pages 回收；仍超 →
     schedule_timeout_killable(penalty_jiffies) ★high 是"节流"不是
     失败——charge 线程被罚睡（TASK_KILLABLE），惩罚时长随超限
     程度增长（源码注释原样核对）
  ④ memory.max 撞死 → MAX_RECLAIM_RETRIES 轮回收
     （try_to_free_mem_cgroup_pages）仍满 → mem_cgroup_out_of_memory
     （本 cgroup 内选牺牲者；v1 的 OOM 旁路=oom_kill_disable 6.6 仍在）
```

## 三、memcg LRU 与 MGLRU（mm/vmscan.c + include/linux/mmzone.h，已核对）

```
每 (memcg, node) 一个 lruvec（mem_cgroup_lruvec/folio_lruvec 已核对）
——回收以 lruvec 为单位，天然按 cgroup 隔离。

经典 LRU（active/inactive 双链）与 MGLRU 并存，运行时按
lru_gen_enabled() 分派（vmscan.c:2917 已核对）：
MGLRU（lru_gen_folio，mmzone.h:416；MIN_NR_GENS=2 / MAX_NR_GENS=4
的代际滑动窗口——源码注释"sliding window of a variable size"）：
  ① 老化（aging）：should_run_aging 判定后 walk_mm()（mmu notifier
     框架下扫页表 accessed bit）+ lru_gen_look_around（缺页点周围
     批量清除）——访问证据从 PTE 直接来，不靠 LRU 位置反推
  ② 驱逐（eviction）：evict_folios 按最老代整批出队
经典路径：isolate_lru_folios（vmscan.c:2304）→ shrink_folio_list
（1705；其内 lru_gen_enabled() 分支做引用修正）——两代机制共用
  shrink_folio_list 的单页决策机
```

## 四、单页决策机：shrink_folio_list（已核对调用形态）

```
对每个 folio 依次过闸：
  ① 映射解除（try_to_unmap 扫反向映射——mm 分册 anon_vma/i_mmap）
  ② 脏 → pageout（先回写再回收；WRITEBACK 中跳过——与 mm 分册
     的 PAGECACHE_TAG_WRITEBACK 账本联动）
  ③ 不可回收（mlocked/在用）→ 放回或转 unevictable 链
  ④ 工作集保护：refault 距离（workingset.c 的影子链）——刚被收回
     又立刻被读回的页获得豁免权重
swap：匿名页走 swap（swappiness；cgroup v2 的 memory.swap.max 独立限额）
```

## 五、slab 与 shrinker：非页内存的回收

```
do_shrink_slab（vmscan.c:834）+ shrink_slab_memcg（932）：
  带 SHRINKER_MEMCG_AWARE（shrinker.h: BIT(3)）的 shrinker
  （dentry/inode 缓存等）按 count_objects/scan_objects 计量回收；
  注册表 shrinker_rwsem 保护（6.6 仍是 rwsem——勘误注：后续内核
  才换成专用 mutex），shrinker_debugfs_add 挂
  /sys/kernel/debug/shrinker/<name>（scan/count 直写触发调试）
对象级计费：obj_cgroup（slab 对象的离线记账，页计费的补充层）
```

## 六、观测与实战

```
memory.stat: pgscan/pgsteal/pgrefill/pgactivate（本 cgroup 回收吞吐）
memory.pressure（v2，PSI：some/full 停顿率）vs v1 memory.pressure_level
memory.high 的节流信号：psi full 高企 + schedule_timeout 罚睡
  （对照组：memory.max 的表现是 OOM 而非延迟）
调试链：高 pgscan 低 pgsteal → 工作集过大或全脏（对照 mm 分册
  writeback 章的脏页节流）；shrinker debugfs 单压 dentry/inode；
  echo 256M > memory.reclaim 做 A/B 验证
MGLRU 开关：/sys/kernel/mm/lru_gen/enabled（min_ttl 等参数同目录）
```

## 七、思考题

1. `memory.high` 用罚睡而 `memory.max` 用 OOM——为什么两种失败语义并存？（提示：延迟可换、OOM 不可换；容器编排里 high 的"软限"用法。）
2. MGLRU 的 aging 靠扫 PTE accessed bit 而非 LRU 链位置——对"顺序流式读"与"随机工作集"分别意味着什么？（对照经典 LRU 的贪婪缺陷。）
3. `walk_mm` 走 mmu notifier 框架——与 KVM 的 tdp_mmu 反向同步（[virt/kvm 02 章](../../virt/kvm/docs/02-EPT与内存虚拟化.md)）是什么协作关系？
4. dentry 缓存的 shrinker 若不申明 MEMCG_AWARE，全局回收时会发生什么？（对照：跨 cgroup 的"误伤回收"与公平性。）
5. `memory.reclaim` 写入 256M 返回值小于请求量说明什么？（提示：工作集保护与不可回收页的存在。）

---

> 回看：[03-memory控制器](03-memory控制器.md)｜mm 分册 [pagecache 与 writeback](../../mm/pagecache/pagecache与writeback.md)｜[KVM EPT 章](../../virt/kvm/docs/02-EPT与内存虚拟化.md)
