# bpf_struct_ops：BPF 实现内核结构体 ops（Linux 6.6）

> 阅读目标：吃透 eBPF 的"反向用法"——**BPF 程序成为内核结构体的函数指针**（而非在内核函数边界旁观）：`bpf_struct_ops` 的注册机制（BTF 驱动的成员校验 + `arch_prepare_bpf_trampoline` 生成每成员的跳板代码）、`tcp_congestion_ops` 作为唯一实战消费端的全流程（BPF 拥塞控制算法 `struct_ops` map → update → trampoline 就位 → `tcp_register_congestion_control`）、`BPF_TRAMP_F_RET_FENTRY_RET` 标志（★trampoline 的"独占用户"）。全部对照 v6.6（kernel/bpf/bpf_struct_ops.c 910 行、include/linux/bpf.h:1553-1625、net/ipv4/bpf_tcp_ca.c、kernel/bpf/bpf_struct_ops_types.h）。前置：[14-BPF trampoline](14-BPFtrampoline.md)（机制基础）。

---

## 一、问题：内核 ops 表的"热加载"需求

```
内核大量子系统用 struct ops 表做策略注入：
  tcp_congestion_ops（拥塞控制）/ scsi_hid等——传统做法：写 C 模块

痛点：
  ① 模块开发/加载/卸载的重门槛（签名/依赖/系统重启）
  ② 运行时不可换（tcp_ca 只在 connect 时选定，不可 mid-flight 换）
  ③ 不可观测（C 函数内部是黑盒）

bpf_struct_ops 的答案：
  创建 BPF_MAP_TYPE_STRUCT_OPS map → 值就是内核 ops 结构体 →
  每个 func ptr 成员填一个 BPF 程序 fd → update 后 trampoline
  把函数指针指向 BPF JIT 代码 → 注册到内核子系统
```

## 二、注册机制：BTF 驱动的三步（kernel/bpf/bpf_struct_ops.c）

### 2.1 类型发现（bpf_struct_ops_init :113-213）

```
启动时（bpf_obj_init→subsys_initcall）：
  bpf_struct_ops_init(btf, NULL)
    └─ BTF_TYPE_EMIT(struct bpf_struct_ops_##_name)  /* 确保 BTF 生成 */
    └─ btf_find_by_name_kind(btf, "bpf_struct_ops_" + name,
                              BTF_KIND_STRUCT)  /* 值类型 */
    └─ btf_find_by_name_kind(btf, st_ops->name, BTF_KIND_STRUCT)
       /* 原型类型（如 "tcp_congestion_ops"）*/
    └─ btf_type_vlen > BPF_STRUCT_OPS_MAX_NR_MEMBERS(64) → 拒绝
    └─ 逐成员 func_models[i] 建立（btf_func_model——与 trampoline 共用）
```

### 2.2 类型注册表（bpf_struct_ops_types.h——X-macro 惯用法的又一实例）

```c
/* kernel/bpf/bpf_struct_ops_types.h —— "internal file, do not include" */
#ifdef CONFIG_BPF_JIT
#ifdef CONFIG_NET
BPF_STRUCT_OPS_TYPE(bpf_dummy_ops)
#endif
#ifdef CONFIG_INET
BPF_STRUCT_OPS_TYPE(tcp_congestion_ops)
#endif
#endif
```

6.6 的**实战类型只有一个**：`tcp_congestion_ops`（加一个测试用的 `bpf_dummy_ops`）。注册表经 `#define BPF_STRUCT_OPS_TYPE(_name) [BPF_STRUCT_OPS_TYPE_##_name] = &bpf_##_name,` 展开为常量表（:95-99）——与 [13 章](13-内核代码生成惯用法.md) 的 lsm_hook_defs 同为**清单单写多消费**的 X-macro 家族。

### 2.3 bpf_struct_ops 结构（bpf.h:1601-1618）

```c
struct bpf_struct_ops {
	const struct bpf_verifier_ops *verifier_ops;
	int  (*init)(struct btf *btf);                  /* BTF 预处理 */
	int  (*check_member)(...);                      /* 成员级校验 */
	int  (*init_member)(...);                       /* 值→内核成员的转换 */
	int  (*reg)(void *kdata);                       /* 注册到内核子系统 */
	void (*unreg)(void *kdata);
	int  (*update)(void *kdata, void *old_kdata);
	int  (*validate)(void *kdata);
	const struct btf_type *type;
	const struct btf_type *value_type;
	const char *name;
	struct btf_func_model func_models[BPF_STRUCT_OPS_MAX_NR_MEMBERS];
	u32 type_id, value_id;
};
```

## 三、struct_ops map：值即内核结构体（bso.c:32-61）

```c
struct bpf_struct_ops_map {
	struct bpf_map map;
	struct rcu_head rcu;
	const struct bpf_struct_ops *st_ops;
	struct mutex lock;
	struct bpf_link **links;      /* 每成员的 link（BPF_F_LINK 模式）*/
	void *image;                  /* ★一页装全部 trampoline 跳板 */
	struct bpf_struct_ops_value *uvalue;  /* 用户视角值（存 prog_id）*/
	struct bpf_struct_ops_value kvalue;   /* ★内核真身（ops 结构体本体）*/
};
```

★**双视图**：`uvalue` 存 BPF 程序 ID（用户可读可写）；`kvalue` 存内核实际函数指针（trampoline 地址）——同一 map 的两种投影。

## 四、update 的全流程：从 fd 到内核函数指针（:369-570+）

```
map_update_elem(map, key, value)  /* value = ops 结构体（fd 字段）*/
  └─ bpf_struct_ops_map_update_elem
       ├─ 逐成员（BTF 反射）：
       │    非函数指针且非零 → -EINVAL（只允许零或 fd）
       │    st_ops->init_member() 委托（如 tcp_ca 的 name/flag 直拷）
       │    常规函数指针：
       │      prog_fd → bpf_prog_get
       │      ★bpf_struct_ops_prepare_trampoline(tlinks, link,
       │        &st_ops->func_models[i], image, image_end)
       │        /* :352-368 */
       │        flags = model->ret_size > 0
       │                 ? BPF_TRAMP_F_RET_FENTRY_RET : 0;
       │        /* 注释原话（:361-362）："BPF_TRAMP_F_RET_FENTRY_RET is
       │           only used by bpf_struct_ops, and it must be used
       │           alone." —— [14 章] 七标志中唯一独占用户 */
       │        arch_prepare_bpf_trampoline(NULL, image, ...,
       │                                     model, flags, tlinks, NULL)
       │        /* orig_call = NULL！—— trampoline 不调任何原函数，
       │           BPF 程序本身就是要执行的全部 */
       │      *(void **)(kdata + moff) = image;  /* 写入 ops 结构体 */
       │      image += err;  /* 推进镜像分配指针 */
       │
       ├─ set_memory_rox(st_map->image, 1)   /* W^X：代码页变只执行 */
       │
       └─ 两种注册路径：
            BPF_F_LINK 模式：smp_store_release(READY) → link 管理
            传统模式：st_ops->reg(kdata) → 子系统注册
```

★`orig_call = NULL` 是与 [14 章](14-BPFtrampoline.md) 的本质区别：fentry/fexit 的 trampoline **包裹**原函数（orig_call 存在），struct_ops 的 trampoline **替代**原函数（无原函数可调）——BPF 程序的返回值就是 ops 成员的返回值（RET_FENTRY_RET 语义）。

## 五、消费端：tcp_congestion_ops（net/ipv4/bpf_tcp_ca.c）

```
BPF 侧（用户态）：定义 BPF_PROG_TYPE_STRUCT_OPS 程序
  SEC("struct_ops/congestion_ssthresh")  /* 名字 = 成员名 */
  BPF_PROG(congestion_ssthresh, struct sock *sk, u32 acked)
  { ... /* C 写法同内核——读 tcp_sock 字段走 BTF */ ... }

加载（内核侧 bpf_tcp_ca.c）：
  bpf_tcp_ca_init :28  记录 sock/tcp_sock 的 BTF id（参数类型提升）
  bpf_tcp_ca_check_member  成员白名单（unsupported_ops 数组除外）
  bpf_tcp_ca_reg  → tcp_register_congestion_control(kdata)
  /* ★注册名来自 ops.name 字段——用户态 BPF 拥塞算法名可自定义 */

使用：
  bpftool struct_ops register bpf_cc.o
  sysctl net.ipv4.tcp_congestion_control=<bpf自定义名>
  → 内核 TCP 连接把 cc_ops 指针指到 BPF trampoline 镜像
```

## 六、观测实战

```bash
# 1) 查看已注册的 struct_ops
bpftool struct_ops show
#   输出形如：
#   Name: bpf_cc  map_id: 42  prog_id: 43

# 2) map 的值（用户视图——fd/ID 而非内核地址）
bpftool map dump id 42

# 3) 拥塞控制的 BPF 实现
sysctl net.ipv4.tcp_congestion_control=bpf_cc

# 4) 源码侧定位
grep -r 'BPF_STRUCT_OPS_TYPE' kernel/bpf/bpf_struct_ops_types.h
#   —— 新 struct_ops 类型加此文件一行

# 5) 性能：trampoline 的零间接调用
#   BPF struct_ops 与 C 模块实现的调用路径等长（都是一次函数指针间接）
```

## 七、思考题

1. `arch_prepare_bpf_trampoline(..., orig_call = NULL)` 与 [14 章](14-BPFtrampoline.md) 的 orig_call 非空有何本质区别？（提示：fentry 是"包夹原函数"，struct_ops 是"替代原函数"——BPF 程序本身是唯一实现。）
2. `BPF_TRAMP_F_RET_FENTRY_RET` 标志"only used by bpf_struct_ops, and must be used alone"——为什么独占？（提示：其他标志组合都假定原函数存在。）
3. struct_ops map 的 `kvalue`（内核真身）与 `uvalue`（用户视图）为什么分开？（提示：用户态如果看到内核函数地址会泄露 ASLR；如果只填 fd，内核无法调用。）
4. `BPF_STRUCT_OPS_MAX_NR_MEMBERS = 64` 的上限为什么存在？（提示：func_models 数组是静态大小；超过则无法预分配 trampoline 布局。）
5. `set_memory_rox(image, 1)` 的时机在 update 流程中——为什么不在 prog load 时就设？（提示：image 是 map 级共享的一页，多次 update 会追加新成员的 trampoline。）
6. `bpf_struct_ops` 只有 `tcp_congestion_ops` 一个实战用户——扩展到其他子系统（如 scsi_request_fn）的障碍是什么？（提示：生命周期（谁持有引用）、上下文限制（BPF 程序不能睡）、参数类型必须 BTF 可表示。）

---

> 回看：[14-BPF trampoline](14-BPFtrampoline.md)（机制的完整基础设施）｜[13-内核代码生成惯用法](13-内核代码生成惯用法.md)（bpf_struct_ops_types 的 X-macro 定位）｜[11-tnum 三值数](11-tnum三值数.md)（verifier 的底层）｜[networking 分册](../../networking/README.md)（tcp_congestion_ops 的 TCP 拥塞控制语境）
