# uclamp 深挖：组级 effective 计算与桶聚合（Linux 6.6）

> 定位：[02-cpu控制器](02-cpu控制器.md) 的 uclamp 执行纵深——从"三层来源"到 effective 计算公式、桶聚合的性能设计、schedutil 的消费路径。前置：02 章、[scheduler/05-PELT](../../scheduler/docs/05-PELT与利用率追踪.md)（util 信号的来源）、[power/02-cpufreq](../../power/docs/02-cpufreq.md)（schedutil）。

---

## 一、util clamp 的四层来源（core.c:1385-1391 注释原样）

```
① 任务级：sched_setattr(SCHED_ATTR_UTIL_CLAMP) → p->uclamp_req[]
② 组级：cgroup v2 cpu.uclamp.{min,max}（cpu_files[] 中
   {.name="uclamp.min"/"uclamp.max"}，CONFIG_UCLAMP_TASK_GROUP，
   CFTYPE_NOT_ON_ROOT——根组不许设）
③ 系统级：sysctl_sched_uclamp_util_{min,max}（默认 1024=不 clamp）
④ RT 专属默认：sysctl_sched_uclamp_util_min_rt_default
   （RT 任务不被 PELT 跟踪——min 默认给满防"RT 任务频控不足"）
```

## 二、effective 计算公式（core.c，已核对 6.6 逐行）

```
任务最终生效值 = 系统天花板 ∩ 组区间(任务请求)：

uclamp_eff_get(p, id):
  uc_req = uclamp_tg_restrict(p, id)     ← 先过组
  if (uc_req.value > uclamp_default[id]) ← 再过系统级
      return uclamp_default[id]

uclamp_tg_restrict(p, id):               /* 组级"夹" */
  uc_req = p->uclamp_req[id]              /* 任务自己的请求 */
  （autogroup 与 root_task_group 豁免——直接返回原值）
  tg_min = task_group(p)->uclamp[UCLAMP_MIN].value
  tg_max = task_group(p)->uclamp[UCLAMP_MAX].value
  value = clamp(uc_req.value, tg_min, tg_max)   ★组把成员请求
                                               夹进组区间
结果缓存进 p->uclamp[id]（uclamp_eff_get 在入队时调，
  uclamp_rq_inc 里 p->uclamp[clamp_id] = uclamp_eff_get(...)）
```

**语义**：`uclamp.min` 是"保底"（组内任务至少被视作 X% util——提频用），`uclamp.max` 是"封顶"（至多 X%——限频用）；effective 取三层的**交**。

## 三、桶聚合：把 O(任务) 压成 O(桶)（include/linux/sched.h，已核对）

```c
#define UCLAMP_BUCKETS CONFIG_UCLAMP_BUCKETS_COUNT   /* 5-20 可配，默认 5 */
struct uclamp_bucket {
	unsigned long value : bits_per(SCHED_CAPACITY_SCALE);  /* 桶内最大值 */
	unsigned long tasks : BITS_PER_LONG - bits_per(...);   /* 桶内任务数 */
};
struct uclamp_rq { unsigned int value;   /* rq 级聚合结果 */
	struct uclamp_bucket bucket[UCLAMP_BUCKETS]; };
```

```
uclamp_bucket_id(v) = v / (1024/UCLAMP_BUCKETS)    /* DELTA≈205 */
入队/出队只增减桶计数 + 必要时更新桶 value（O(1)）
rq 聚合 = 所有桶里的最大 value（uclamp_rq_get 读 rq->uclamp[id].value）
零开销门：sched_uclamp_used 静态键（DEFINE_STATIC_KEY_FALSE）——
  没人用过 uclamp 时入队路径完全跳过；首次写 cpu.uclamp.* 时
  static_branch_enable（cpu_uclamp_write 内原句核对）
```

## 四、消费侧：schedutil 与 EAS

```
schedutil 提频路径（power/02 章的 effective_cpu_util）：
  util = uclamp_rq_util_with(rq, util, p)   /* core.c:7442 已核对 */
  ——min clamp 抬底：后台音频线程 util=3% 但 uclamp.min=15%
     → 频率按 15% 起算（保实时性场景）
  ——max clamp 压顶：批处理任务突发满 util 但 uclamp.max=50%
     → 频率天花板被压（防干扰同机前台）
EAS（sched_energy_annotate? 见 scheduler 分册）：capacity 判定同样
  吃 clamp 后的 util——uclamp.max 大时任务被判"放得进小核"
```

## 五、写路径与层级传播

```
echo 20 > cpu.uclamp.min:
  cpu_uclamp_write: capacity_from_percent("20") → util=205
  → static_branch_enable(&sched_uclamp_used)（首次）
  → uclamp_mutex + rcu_read_lock 下写 tg->uclamp_req[MIN]
  → 触发 uclamp_update：任务组的 uc_se 变更沿层级向上传播
    （父组 effective 受子组请求影响——限制的传递性；
    对已在运行任务的批量重算：for_each_process_thread 走
    uclamp_change_cpu? — 6.6 的实现走 cpu.cgroup_attach/迁移
    或 task 遍历重算 p->uclamp）
```

## 六、观测与实战

```
cat cpu.uclamp.min / cpu.uclamp.max      # 组区间（0=不设下限，
                                          1024=满额）
sysctl kernel.sched_uclamp_util_min_rt_default  # RT 保底策略
压测对照：音频任务 {uclamp.min=15%} vs 无 clamp 的频轨
  （trace-cmd 抓 cpufreq:schedutil_util 插值差异）
排障：uclamp.min 不生效 → 检查任务是否落在受限子组
  （uclamp_tg_restrict 的组区间是否吞掉了任务请求）
```

## 七、思考题

1. `uclamp_tg_restrict` 对 root_task_group 与 autogroup 豁免——为什么根组与 autogroup 必须直通？（提示：autogroup 无 kernfs 接口可写；根组 CFTYPE_NOT_ON_ROOT 的语义对齐。）
2. 桶数默认 5 而非精确 1024 级——聚合误差出现在哪类工作负载？（对照 DELTA≈205 的粒度 vs 任务 util 分布。）
3. `sched_uclamp_used` 静态键为什么用"写接口触发"而非编译期判断？（对照 CONFIG_UCLAMP_TASK 组合下的运行时灵活性。）
4. RT 任务的 min 默认 1024 的动机？（提示：RT 不参与 PELT，util 恒 0 的频控后果。）
5. uclamp.max=50% 的批处理任务放进小核——EAS 判定与 schedutil 提频的交互里，"util 夹逼"发生在哪一步？（对照 §四 两个消费点。）

---

> 回看：[02-cpu控制器](02-cpu控制器.md)｜[05-memcg 回收深挖](05-memcg回收深挖.md)｜[scheduler/05-PELT](../../scheduler/docs/05-PELT与利用率追踪.md)｜[power/02-cpufreq](../../power/docs/02-cpufreq.md)
