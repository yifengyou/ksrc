# Yama

## 源代码路径

```text
# ls -ahl security/yama/
total 24K
drwxr-xr-x  2 root root  55 Nov 17 09:08 .
drwxr-xr-x 11 root root 278 Nov 17 12:30 ..
-rw-r--r--  1 root root 491 Nov  7 14:07 Kconfig
-rw-r--r--  1 root root  60 Nov  7 14:07 Makefile
-rw-r--r--  1 root root 13K Nov  7 14:07 yama_lsm.c
```


## 相关配置

```text
CONFIG_SECURITY_YAMA=y
此选项会选择 Yama。Yama 会利用其他系统范围的安全设置，将 DAC 支持扩展到常规 Linux 自主访问权限控制范围之外。 目前，该设置受限于 ptrace 范围。

CONFIG_SECURITY_YAMA_STACKED=y
在 Yama 可用时，该选项会强制 Yama 与选定的主 LSM 堆叠。
```


## 原理

Yama主要是对Ptrace函数调用进行访问控制。

Ptrace是一个系统调用，它提供了一种方法来让‘父’进程可以观察和控制其它进程的执行，检查和改变其核心映像以及寄存器。 主要用来实现断点调试和系统调用跟踪。
利用ptrace函数，不仅可以劫持另一个进程的调用，修改系统函数调用和改变返回值，而且可以向另一个函数注入代码，修改eip，进入自己的逻辑。
这个函数广泛用于调试和信号跟踪工具。所以说，对ptrace函数进行访问控制还是很有必要的。

Yama一共分为四个等级：


```text
#define YAMA_SCOPE_DISABLED    0
#define YAMA_SCOPE_RELATIONAL    1
#define YAMA_SCOPE_CAPABILITY    2
#define YAMA_SCOPE_NO_ATTACH    3
```


其中YAMA_SCOPE_DISABLED代表yama并不起任何作用，
YAMA_SCOPE_RELATIONAL代表只能ptarce子进程才能进行调试，YAMA_SCOPE_CAPABILITY，拥有CAP_SYS_PTRACE能力的进程才可以使用ptrace。而YAMA_SCOPE_NO_ATTACH代表没有任何进程可以attach，而且只要设置成了3就无法降级了。

现在，先来测试使用一下，先将等级设为0。在root权限下进行：

```shell
[root@linux ~]# ls /proc/sys/kernel/yama/ptrace_scope 
/proc/sys/kernel/yama/ptrace_scope
[root@linux ~]# cat $_
0
[root@linux ~]# 
```














---