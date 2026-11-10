# livepatch 一致性模型与 transition（Linux 6.6）

> 阅读目标：掌握 livepatch 的**逐任务一致性模型**——为什么不能在有任务睡在被补丁函数的栈帧上时直接切换、`klp_target_state`/`task->patch_state` 双状态与 `TIF_PATCH_PENDING` 的协作、`klp_try_switch_task` 的完整协议（串行化 + 栈检查 + 唤醒）、原子替换（replace/nop）、系统状态（klp_state）与影子变量。前置：[klp 核心结构](01-klp核心结构与打补丁流.md)、[trace 分册 kprobes](../../trace/docs/04-kprobes与uprobes.md)（可靠的栈回溯从哪来）。

---

## 一、问题：睡在旧代码上的任务

```
补丁生效 = ftrace 入口改派发（上一章 klp_ftrace_handler 的 transition 分支）
风险场景：任务 A 已在 old_func 的栈帧里 sleep——醒来后从 old_func
  中段继续执行（此时新代码已在函数入口生效）：
  ① 老栈帧访问的数据结构可能被补丁改语义 → 崩
  ② old_func 返回后走 old 的调用链 → 新旧语义交错
结论：A 必须先"走出"所有被补丁函数，才能承认它处于新世界
     —— 这就是逐任务（per-task）一致性模型
```

## 二、双状态与标志位（kernel/livepatch/transition.c，已核对 6.6）

```
klp_target_state（全局）：本轮 transition 的目标 KLP_PATCHED / KLP_UNPATCHED
task->patch_state（per-task）：该任务当前承认的世界
TIF_PATCH_PENDING（per-task）：该任务尚未跨过本轮门槛
协议（klp_try_switch_task 已核对）：
  task_call_func(func, arg) 串行化 —— 任务持 task_lock 执行回调，
  睡着的任务在 try_to_wake_up 的同一锁域里被改标志（源码注释：
  "Thanks to task_call_func they won't be missed"）
  if (test_and_clear_tsk_thread_flag(task, TIF_PATCH_PENDING))
      task->patch_state = klp_target_state
  入口（schedule/resched/信号返回路径）检查 TIF，有 PENDING 则
  klp_patch_task(current) 先切自己再干活 —— 每个任务自己跨门槛
```

## 三、栈检查：klp_check_stack（已核对 6.6）

```
klp_try_switch_task(task) 对 running 任务：
  ① 抓栈：unwinder 走 task 的内核栈（unwind_start/entries，
     依赖 CONFIG_HAVE_RELIABLE_STACKTRACE——x86 用 ORC/unwind_orc）
  ② klp_check_stack_func(func, entries, nr_entries)：
     任一栈帧 IP 落在 [old_func, old_func+old_size) → 该任务
     "睡在被补丁函数里"（返回 oldname 供诊断）
  ③ 干净 → 切 patch_state；脏 → -EBUSY，发 signal 唤醒
     （signal_pending 路径，transition stall 场景的救援通道）
  ④ klp_dump_stack：诊断模式下把整栈打出来（哪个函数卡了 transition）
transition 完成（klp_try_complete_transition）：所有任务
  patch_state == klp_target_state → 收尾：清 transition_patch、
  func->transition=false（ftrace 派发从此无条件走新地址）、
  空闲任务由 idle 帮忙跨门槛（cpu offline 兼容）
```

## 四、原子替换与系统状态

```
replace 模式（klp_patch.replace=1，"累计补丁"官方姿势）：
  新补丁对旧补丁覆盖过的每个函数生成 nop 补丁体（func->nop）——
  ftrace func_stack 弹掉旧层，行为回到老内核
  transition 完成后 klp_unpatch_objects_static 把旧补丁整体下架
  （core.c 已核对：replace && KLP_PATCHED 分支）
klp_state（patch->states）：
  补丁声明"我引入了系统状态 X"——后续补丁必须声明承接，
  防止语义不兼容的补丁叠加（system-state.rst 的机制化）
klp_callbacks：pre_patch/post_patch/pre_unpatch/post_unpatch
  —— 比如补丁需要先做数据结构迁移（callbacks.rst 的标准用法）
影子变量（shadow.c）：klp_shadow_alloc/get/free/dump —— 给被补丁
  结构体挂"补丁私有字段"（不改老结构体布局，卸载时带走）
```

## 五、观测与实战

```
/sys/kernel/livepatch/<p>/transition      # 1=过渡进行中
/sys/kernel/livepatch/<p>/force           # WO：卡死时强收（丢一致性换可用）
dmesg: livepatch: 'xxx': starting patching transition ...
       卡住会打 "%d tasks are stuck" 与 klp_dump_stack 输出
排查三板斧：
  ① cat transition 长期 1 → 找 stuck 任务（dmesg 栈）：
     通常是长期睡在被补丁函数里的内核线程（mutex_wait 之类）
  ② 对方是否在可靠栈回溯架构上（无 HAVE_RELIABLE_STACKTRACE
     的架构 livepatch 直接不可用）
  ③ force 前先评估丢一致性的后果（睡栈任务醒来仍走旧代码）
```

## 六、思考题

1. 为什么不用 stop_machine 一次性切换？（对照 6.6 之前的 immediate 标志为何被删除：停机窗口对延迟敏感负载的代价 vs transition 的收敛等待。）
2. `test_and_clear_tsk_thread_flag` 与 `task->patch_state` 的先后顺序为什么不能反？（推演交错时的瞬时双世界。）
3. `klp_check_stack` 为什么必须依赖"可靠"栈回溯——帧指针回溯的漏帧会导致什么？（提示：漏掉一帧 = 把睡在补丁函数里的任务判成干净。）
4. nop 补丁为什么必须占用一个真实 klp_func 而不是直接撤销 ftrace ops？（对照 func_stack 的栈序与并发在栈上执行的旧补丁代码。）
5. `replace` 模式下新旧补丁的 klp_state 冲突时 refuse 的判据是什么？（对照 system-state 的版本号承接规则。）

---

> 回看：[klp 核心结构与打补丁流](01-klp核心结构与打补丁流.md)｜官方镜像：[reliable-stacktrace](../reliable-stacktrace.rst)、[shadow-vars](../shadow-vars.rst)
