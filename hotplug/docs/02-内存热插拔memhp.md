# 内存热插拔 memhp：block/zone/pgdat 的动态生灭（Linux 6.6）

> 阅读目标：掌握内存热插拔的三层模型（resource → memory block → zone/pgdat）、`add_memory_resource` 的建立链（sparse section + altmap + `move_pfn_range_to_zone`）、**memmap_on_memory 自托管 vmemmap**、`offline_pages` 的完整下线协议（pcp 关闭 → 隔离 → 迁移 → 收缩 → 回滚路径）。前置：[mm 分册 buddy](../mm/buddy/buddy.md)、[hotplug/cpuhp](01-cpuhp状态机.md)。

---

## 一、三层模型

```
① resource：新内存以 iomem 资源注入（"System RAM"）
② memory block：管理粒度（/sys/devices/system/memory/memoryN/）
   block_size_bytes 决定粒度（X86_64 通常 128MB/块，也可是
   section 大小的倍数）；state: offline→online；valid_zones
③ zone/pgdat：online 时挂进 zone（通常 ZONE_MOVABLE 或默认 zone），
   新 node 场景则 hotadd_new_pgdat 重建整套
```

## 二、上线链：add_memory → online（mm/memory_hotplug.c，已核对 6.6）

```
add_memory(nid, start, size, mhp_flags) → add_memory_resource(nid, res, mhp_flags):
  mhp_t 旗标（memory_hotplug.h 已核对）：
    MHP_NONE=0 / MHP_MERGE_RESOURCE=BIT(0) / MHP_MEMMAP_ON_MEMORY=BIT(1)
  ① memblock 添加 + sparse_add_section(nid, pfn, nr_pages, altmap)
     （altmap：vmemmap 放哪的"备选地图"，见下）
  ② create_memory_block_devices → sysfs 露出 memoryN
  ③ （默认 offline）等用户态/策略 online：
     online_pages → move_pfn_range_to_zone(zone, pfn, nr_pages, NULL,
       MIGRATE_ISOLATE)  ← 先按 ISOLATE 挂进 buddy，再
       undo_isolate_page_range(..., MIGRATE_MOVABLE) 整块放行
     zone->present_pages/managed_pages += nr_pages（源码行已核对）
     新 node：build_all_zonelists + node_states_set（N_MEMORY/N_CPU）
memhp_default_state=online_start（boot 参数，__setup 已核对）与
  /sys/devices/system/memory/auto_online_blocks 等价运行时版
```

## 三、memmap_on_memory：自托管 vmemmap（6.6 主线特性）

```
问题：每上 128MB 内存需要 ~2MB vmemmap（struct page 数组）——
  传统方案从既有内存借（hot-add 之前就得有地方放描述符）
自托管（CONFIG_MHP_MEMMAP_ON_MEMORY + memory_hotplug.memmap_on_memory
  模块参数，0444 已核对）：把新块**头部几页**当 vmemmap 用——
  mhp_init_memmap_on_memory(pfn, nr_pages)：块首 memmap_pages 初始化
  成描述"块内其余页"的 struct page（altmap 指向块内）
代价与对齐：vmemmap 尺寸须 pageblock 对齐——"forced" 模式宁可
  多吃对齐填充也要成块（源码注释核对）；无法整块自托管的回退普通
  vmemmap。与 hugetlb HVO（thp 分册提过）同源思想：struct page 的
  存储成本由被描述内存自己承担
移除约束：仅 MHP_MEMMAP_ON_MEMORY 添加的块支持整块 remove
  （源码注释核对：vmemmap 藏在块内，块走了描述符也走）
```

## 四、下线协议：offline_pages（已核对 6.6 全流程）

```
offline_pages(start_pfn, nr_pages, zone, group):
  ① 对齐与一致性检查：pageblock 对齐 + SECTION 对齐 +
     walk_system_ram_range 验无洞（"memory holes"）+ 起止同 zone
  ② mem_hotplug_begin（全局热插拔互斥）
  ③ zone_pcp_disable(zone)         ← per-CPU 页表冻结（buddy 分册 pcp）
  ④ start_isolate_page_range(start_pfn, end_pfn, MIGRATE_MOVABLE,
       MEMORY_OFFLINE)             ← pageblock 标 ISOLATE，buddy 视而不见
  ⑤ node_states_check_changes_offline（N_HIGH_MEMORY 门槛核算）
  ⑥ do_migrate_range(pfn, end_pfn) ← 把页迁走（含 unmovable 判定：
     迁不动 → "failure to migrate range"）
  ⑦ test_pages_isolated(...)       ← 复核区间真空
  ⑧ 干净：memmap on/offline 收尾 + shrink_zone_span(zone, …)
     （zone/pgdat 的 span 收缩，源码行核对）+ present_pages 递减 +
     node_states_clear_node + kdump/伙伴系统联动
  失败回滚（failed_removal_isolated）：
     undo_isolate_page_range → zone_pcp_enable → 恢复 online
     —— 热插拔的"事务性"：任何一步失败都退回原状
```

`offline_pages` 只处理"真空块"；把块从 sysfs/resource 层抹掉走 `remove_memory`（arch 依赖）。

## 五、观测与实战

```
/sys/devices/system/memory/
  memoryN/state, valid_zones, block_size_bytes, auto_online_blocks
cat /proc/zoneinfo        # present/managed 页数变化（对照上线前后）
dmesg | grep -e 'memory hotplug' -e 'Offline pages'
失败诊断三板斧：
  ① "memory holes"：块跨了非 RAM 区 → 粒度/对齐问题
  ② "failure to isolate"：块内 pageblock 无法全部 ISOLATE
     （多因：有 kernel 画像/CRITICAL 页在内）
  ③ "failure to migrate"：unmovable 页钉死（pfn dump）——
     常见为 slab/页表/CMA 之外的非迁移页落在目标区
工具：echo offline > …/memoryN/state；KASAN/PAGE_OWNER 帮助定位钉子页
```

## 六、思考题

1. 上线为什么先 `MIGRATE_ISOLATE` 挂 buddy 再整体放行为 MOVABLE？（提示：中途可见的半初始化区间若被分配器拿走的后果。）
2. memmap_on_memory 的块为何天然支持整块 remove 而普通块要小心 vmemmap 残留？（对照 altmap 与块内自描述。）
3. `zone_pcp_disable` 若被忘记会发生什么？（推演 pcp 里的"幽灵页"：isolated 判定后又被本 CPU 吐回分配。）
4. offline 失败回滚后 zone 的 span/present_pages 必须完全还原吗？（对照 shrink 只在成功路径执行。）
5. `memhp_default_state=online` 与"先离线再上"策略在 kdump/迁移场景的差异？（提示：online 时机决定块进 MOVABLE 还是默认 zone。）

---

> 回看：[cpuhp 状态机](01-cpuhp状态机.md)｜[mm 分册 buddy](../mm/buddy/buddy.md)｜[trace 分册](../trace/README.md)
