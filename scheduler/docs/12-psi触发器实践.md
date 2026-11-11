# psi 触发器实践（Linux 6.6）

> 定位：[09-PSI 压力失速信息](09-PSI压力失速信息.md) 的触发器纵深——谓词模型、参数校验链、fd 的 poll 语义、rtpoll 引擎与用户态集成模板。前置：09 章。

---

## 一、触发器模型：把"趋势判断"搬进内核

```
avg 轮询的用户态困境：采样误差 + 判定迟滞（等 avg 爬过阈值时
  失速已持续多窗）
触发器谓词："窗口 W 内累计失速 ≥ threshold 微秒"——内核逐窗
  精确判定，达界即事件（用户态 poll() 即醒）
写法（/proc/pressure/<res> 或 cgroup 的 <res>.pressure）：
  echo "some 150000 1000000" > memory.pressure
  → 返回一个 fd（psi_trigger 的句柄）
```

## 二、创建与参数校验链（psi.c:1290-1319，已核对）

```
解析（sscanf 原句）："some %u %u" / "full %u %u"
  → (state, threshold_us, window_us)
校验四规则（逐条命中源码）：
  ① window_us == 0 或 > WINDOW_MAX_US(10000000=10s) → EINVAL
  ② ★非特权（无 CAP_SYS_RESOURCE）：window_us % 2000000 != 0
    → EINVAL——普通用户只能用 2s 整倍窗（防细窗高频 rtpoll
    打爆内核；privileged = cap_raised(f_cred->cap_effective,
    CAP_SYS_RESOURCE) :1290）
  ③ threshold_us == 0 或 > window_us → EINVAL（阈值必在窗内）
  ④ 状态合法性（NONIDLE 以上、IRQ 的 full 特判）
```

## 三、fd 语义：poll 即醒（psi_trigger_poll:1465-1489，已核对）

```
poll(fd)：
  ★EPOLLPRI 为事件位——且是**边沿消费**：
    if (cmpxchg(&t->event, 1, 0) == 1) ret |= EPOLLPRI
    （读走即清零——一次事件一次唤醒，天然防重报）
  等待点：普通 fd 走 t->event_wait；cgroup 文件型（t->of）
    走 kernfs_generic_poll——两路统一在 poll 回调里分流
  出错位：psi_disabled 或 trigger 已被替换 → EPOLLERR|EPOLLPRI
替换语义：再次写同一文件 = 替换触发器（旧 t 经 RU + kthread_stop
  销毁——:1440-1445 的 destroy 链）
```

## 四、rtpoll 引擎：内核侧的逐窗判定（已核对）

```
per-group 内核线程 psi_rtpoll_worker（:723-736）跑
  psi_rtpoll_work（:639）：
    按组内最短窗调度（rtpoll_min_period——多触发器共享一 worker）
    每窗 UPDATES_PER_WINDOW=10 次采样（:164）——判定"累计失速
    是否已达 threshold"在窗内即报，不等窗满
组级调度：psi_schedule_rtpoll_work（:612）的 delay 计算；
  无触发器时 worker 完全不跑（零常态开销）
```

## 五、用户态集成（用户态事实，框架性描述）

```
模板（epoll 循环）：
  fd = open("/sys/fs/cgroup/.../memory.pressure")
  write(fd, "full 500000 2000000") → 返回 trigger fd
  epoll 注册（EPOLLPRI）→ 事件即动作（scale/kill/告警）
生态：oomd（Meta）与 systemd-oomsd 用 memory.full/full 阈值
  触发用户级 OOM 杀伐——PSI 触发器是其核心输入
最佳实践：阈值参考"业务可容忍停顿率"（如 full 500ms/2s=25%
  完全失速再动手），而非直接抄 avg 数值
```

## 六、与 avg 轮询对比

| | avg 轮询（读 .pressure） | 触发器（write+poll） |
| :--- | :--- | :--- |
| 判定位置 | 用户态 | 内核逐窗 |
| 精度 | 采样误差 | 精确（µs 累计） |
| 迟滞 | avg 平滑拖慢 | 窗内即报 |
| 开销 | 持续读 | 无事零开销（worker 不跑） |
| 粒度 | 10/60/300s 三档 | 自定窗（特权可 1ms..10s） |

## 七、思考题

1. 非特权限 2s 整倍窗防的是什么 DoS？（对照 rtpoll worker 的调度频率与组数量。）
2. `cmpxchg(&t->event, 1, 0)` 的边沿消费为什么优于电平触发？（重报风暴 vs 漏报的取舍。）
3. threshold ≤ window 的约束背后是什么物理意义？（"窗内失速占比 ≤100%"。）
4. oomd 选 full 而非 some 触发杀伐——容量与延迟的哪个语义？（对照 09 章 SOME/FULL 分野。）
5. 多触发器共享一个 rtpoll worker——最短窗者的公平性代价？（长窗触发器的判定密度被抬高。）

---

> 回看：[09-PSI 压力失速信息](09-PSI压力失速信息.md)｜[00-一页速查](00-一页速查.md)｜[cgroup/05-memcg 回收深挖](../../cgroup/docs/05-memcg回收深挖.md)
