# SELinux模块

```text
module myapp 1.1;

########################################
# 1. 声明需要引用的系统现有类型和类 (Require Section)
########################################
require {
    # 引用系统已有的对象类
    class file { read open getattr execute entrypoint };
    class process { signal sigchld transition };
    
    # 引用系统已有的属性 (如果这些属性在系统中已存在，必须在这里require)
    # 注意：有些系统可能不需要require attribute本身，但为了兼容性最好加上
    attribute domain;
    attribute file_type;
    attribute exec_type;
}

########################################
# 2. 定义本模块的新类型
########################################
type myapp_t;
type myapp_exec_t;
type myapp_log_t;

########################################
# 3. 赋予属性 (Typeattributes)
########################################
# 将 myapp_t 标记为域
typeattribute myapp_t domain;

# 将 myapp_exec_t 标记为文件类型和可执行类型
typeattribute myapp_exec_t file_type;
typeattribute myapp_exec_t exec_type;

# 将 myapp_log_t 标记为文件类型
typeattribute myapp_log_t file_type;

########################################
# 4. 定义访问规则 (Allow Rules)
########################################

# 允许域转换相关的文件权限
allow myapp_t myapp_exec_t:file { entrypoint open read getattr execute };

# 允许基本的进程操作
allow myapp_t self:process { signal sigchld transition };

# 允许 myapp_t 读取日志 (这里用到了 require 中声明的 file 类)
allow myapp_t myapp_log_t:file { read open getattr };

```






```text

# 检查语法
checkmodule -M -m -o myapp.mod myapp.te

# 将 .mod 转换为 .pp
semodule_package -o myapp.pp -m myapp.mod

# 安装策略模块
semodule -i myapp.pp


```