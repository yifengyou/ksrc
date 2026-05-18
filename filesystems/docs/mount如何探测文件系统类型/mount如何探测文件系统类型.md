# mount如何探测文件系统类型

```text
mount命令
  ↓
libmount/libblkid 探测文件系统
  ↓
mount(2) 系统调用
  ↓
VFS
  ↓
查找文件系统驱动(ext4/xfs...)
  ↓
读取superblock验证
  ↓
挂载成功
```

底层mount系统调用接口，需要提供文件系统类型

```text
NAME
       mount - mount filesystem

LIBRARY
       Standard C library (libc, -lc)

SYNOPSIS
       #include <sys/mount.h>

       int mount(const char *source, const char *target,
                 const char *filesystemtype, unsigned long mountflags,
                 const void *_Nullable data);
```

示例：

```c
mount("/dev/sda1",
      "/mnt",
      "ext4",
      0,
      NULL);
```













































































































































---

