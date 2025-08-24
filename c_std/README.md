# kernel c standard


## 内核C标准

Linux 内核从 5.18 起正式迁移到了 C11（GNU11）标准，之前长期使用的是 C89（GNU89）

* -std=gnu90 (for C90 with GNU extensions)
* -std=gnu99 (for C99 with GNU extensions)
* -std=gnu11 (for C11 with GNU extensions)

```
[root@7945HX /linux/linux-6.git]# grep std= Makefile
			 -O2 -fomit-frame-pointer -std=gnu11
KBUILD_CFLAGS += -std=gnu11
```

```
[root@7945HX /linux/linux-5.git]# grep std= Makefile
			      -O2 -fomit-frame-pointer -std=gnu89
		   -std=gnu89
# The kernel builds with '-std=gnu89' so use of GNU extensions is acceptable.
```

```
[root@7945HX /linux/linux-4.git]# grep std= Makefile
		-fomit-frame-pointer -std=gnu89 $(HOST_LFS_CFLAGS)
		   -std=gnu89
```

```
[root@7945HX /linux/linux-3.git]# grep std= Makefile
HOSTCFLAGS   = -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89
		   -std=gnu89
```


## 检查方法

打开内核源码根目录下的 Makefile，搜索如下变量：

```
KBUILD_CFLAGS += -std=gnu11
```

- 如果看到的是 gnu11，说明内核使用 C11 标准。
- 如果看到的是 gnu89，说明内核使用 C89 标准。
- 适用于内核 5.18 及以后版本，例如 Linux 6.6 使用的是 -std=gnu11。

## GNU11跟ISO C11区别

GNU11在ISO C11基础上添加了大量**编译器专属扩展**，这些特性在Linux内核、驱动开发中广泛使用，但不符合ISO标准：

| **特性类型**       | **GNU11扩展示例**                          | **ISO C11是否支持** |
|--------------------|--------------------------------------------|---------------------|
| **零长度数组**     | `char data[0];`（柔性数组成员）            | ❌ 不支持            |
| **语句表达式**     | `({ int x=1; x++; })`（返回最后一个表达式值） | ❌ 不支持            |
| **属性声明**       | `__attribute__((packed))`（取消结构体对齐） | ❌ 不支持            |
| **内联汇编**       | `asm("mov %eax, 1")`（直接嵌入汇编指令）    | ❌ 不支持            |
| **Case范围**       | `case 1 ... 5:`（匹配1至5）                | ❌ 不支持            |
| **标号元素**       | `int arr[10] = { [3]=1, [7]=2 };`          | ⚠️ C99起部分支持    |
| **`typeof` 表达式**  | 动态获取变量类型   `typeof(x) y = x + 1;`  | ❌ 不支持      |