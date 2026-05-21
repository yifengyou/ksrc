# landlock

Landlock 是 Linux 内核中一个**轻量级、无特权（unprivileged）的沙箱机制**，属于 **Linux 安全模块（LSM, Linux Security Module）** 的一种实现。它从 Linux 5.13 开始逐步引入，并在 5.15 中趋于稳定。其目标是为应用程序提供一种**自主限制自身权限的能力**，从而增强系统安全性，尤其适用于容器、浏览器、AI Agent 等需要隔离运行环境的场景。

---

## 一、核心作用

Landlock 的主要功能是：

- **限制进程对文件系统的访问权限**（后续版本计划支持网络等其他资源）。
- 允许**非 root 用户**创建安全策略（即“无特权沙箱”），无需依赖管理员或复杂配置。
- 实现**最小权限原则（Principle of Least Privilege）**：即使程序被攻破，攻击者也无法访问未授权的资源。

例如，一个 Web 浏览器可以使用 Landlock 限制自己只能访问 `/tmp` 和用户下载目录，而无法读取 `/etc/passwd` 或主目录中的敏感文件。

---

## 二、技术原理

### 1. 基于 LSM 框架

Landlock 是标准 LSM 模块，但与其他 LSM（如 SELinux、AppArmor）不同：
- **不需要 root 权限**即可启用；
- **由用户空间程序主动调用**，而非系统全局策略；
- **叠加式（stackable）**：可与其他 LSM 共存，权限取交集（更严格）。

### 2. 使用 eBPF 技术（间接）

虽然早期有讨论使用 eBPF 表达策略，但最终 Landlock **并未直接依赖 eBPF 虚拟机**。其规则通过内核提供的专用系统调用（如 `landlock_create_ruleset`、`landlock_add_rule`）定义，由内核内部高效处理。

### 3. 规则模型

- **Ruleset（规则集）**：一组访问控制规则的集合。
- **Rule（规则）**：指定某个路径（或 inode）允许哪些操作（如读、写、执行、挂载等）。
- **Domain（域）**：将 ruleset 应用于当前进程及其子进程，形成“沙箱”。

一旦进入 Landlock 域，进程**只能访问规则明确允许的路径**，且**不能退出该域**（单向约束）。

---

## 三、关键系统调用（用户空间 API）

Landlock 提供以下系统调用（自 Linux 5.13 起）：

| 系统调用 | 功能 |
|--------|------|
| `landlock_create_ruleset()` | 创建一个新的规则集 |
| `landlock_add_rule()` | 向规则集中添加路径访问规则 |
| `landlock_restrict_self()` | 将当前进程限制到该规则集（不可逆） |

> 注意：这些调用通常通过 libc 或高级语言封装库（如 Rust 的 `landlock` crate）使用。

---

## 四、典型应用场景

1. **桌面应用沙箱**：如 Firefox、Chromium 可限制插件访问范围。
2. **容器运行时**：作为 seccomp、namespaces 的补充，提供细粒度文件访问控制。
3. **AI Agent 安全**：防止 LLM 调用的工具访问敏感系统文件。
4. **服务降权**：Web 服务器在启动后主动限制自身文件访问能力。

---

## 五、与传统安全机制对比

| 机制 | 是否需 root | 粒度 | 易用性 | 适用场景 |
|------|------------|------|--------|----------|
| **Landlock** | ❌ 否 | 路径级 | 高（程序自控） | 应用级沙箱 |
| **SELinux** | ✅ 是 | 文件/进程标签 | 低（策略复杂） | 企业级系统安全 |
| **AppArmor** | ✅ 是 | 路径/程序 | 中 | Ubuntu 默认方案 |
| **seccomp** | ❌ 否 | 系统调用 | 中 | 限制 syscalls |
| **chroot** | ✅ 是 | 目录 | 低（易逃逸） | 基础隔离 |

---

## 六、示例（伪代码）

```c
// 1. 创建规则集（允许读写 /tmp）
int ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);

// 2. 添加规则：允许访问 /tmp
landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                  &path_beneath, 0);

// 3. 应用限制（不可逆！）
prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
landlock_restrict_self(ruleset_fd, 0);
```

此后，进程只能访问 `/tmp` 及其子目录。

---

## 七、局限性

- 目前仅支持**文件系统访问控制**（未来可能扩展网络、IPC 等）。
- 不支持动态撤销规则（一旦限制，无法放宽）。
- 需要应用程序主动集成（非透明防护）。

---

## 参考资料

- [官网](https://landlock.io)
- [Linux 内核官方文档(英文) - Landlock](https://docs.kernel.org/security/landlock.html)
- [Linux 内核官方文档(中文) - Landlock](https://www.kernel.org/doc/html/next/translations/zh_CN/security/landlock.html)
- [Landlock 用户空间 API 文档](https://www.kernel.org/doc/html/latest/userspace-api/landlock.html)

---

## 源码统计

```shell
# wc -l *
   20 common.h
   53 cred.c
   58 cred.h
   87 errata.h
 1319 fs.c
   95 fs.h
   21 Kconfig
   27 limits.h
    4 Makefile
   67 object.c
   91 object.h
  120 ptrace.c
   14 ptrace.h
  475 ruleset.c
  180 ruleset.h
   71 setup.c
   21 setup.h
  482 syscalls.c
 3205 总计
 # cloc .
      18 text files.
      17 unique files.                              
       1 file ignored.

github.com/AlDanial/cloc v 1.98  T=0.02 s (1005.1 files/s, 188245.0 lines/s)
-------------------------------------------------------------------------------
Language                     files          blank        comment           code
-------------------------------------------------------------------------------
C                                7            285            792           1510
C/C++ Header                     9             84            277            232
make                             1              1              0              3
-------------------------------------------------------------------------------
SUM:                            17            370           1069           1745
-------------------------------------------------------------------------------

```
