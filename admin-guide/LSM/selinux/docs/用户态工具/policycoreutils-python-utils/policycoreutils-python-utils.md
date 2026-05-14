<!-- vscode-markdown-toc -->
* 1. [软件包信息](#)
* 2. [基本使用](#-1)
	* 2.1. [semanage](#semanage)
	* 2.2. [audit2allow](#audit2allow)
	* 2.3. [audit2why](#audit2why)
	* 2.4. [chcat](#chcat)
* 3. [软件源码](#-1)

<!-- vscode-markdown-toc-config
	numbering=true
	autoSave=true
	/vscode-markdown-toc-config -->
<!-- /vscode-markdown-toc -->

# policycoreutils-python-utils

##  1. <a name=''></a>软件包信息

```shell
[root@openeuler6 ~]# yum info policycoreutils-python-utils
Last metadata expiration check: 0:00:15 ago on Mon 27 Apr 2026 09:07:17 AM CST.
Installed Packages
Name         : policycoreutils-python-utils
Version      : 3.5
Release      : 4.oe2403sp2
Architecture : noarch
Size         : 80 k
Source       : policycoreutils-3.5-4.oe2403sp2.src.rpm
Repository   : @System
From repo    : OS
Summary      : Policy core python utilities for selinux
URL          : https://github.com/SELinuxProject
License      : GPLv2
Description  : It contains the python utilities for selinux

[root@openeuler6 ~]# rpm -ql policycoreutils-python-utils
/etc/dbus-1/system.d/org.selinux.conf
/etc/ima/digest_lists.tlv/0-metadata_list-compact_tlv-policycoreutils-python-utils-3.5-4.oe2403sp2.noarch
/etc/ima/digest_lists/0-metadata_list-compact-policycoreutils-python-utils-3.5-4.oe2403sp2.noarch
/usr/bin/audit2allow
/usr/bin/audit2why
/usr/bin/chcat
/usr/sbin/semanage
/usr/share/bash-completion/completions/semanage
[root@openeuler6 ~/rpmbuild/BUILD]# cloc .
    1827 text files.
    1108 unique files.                                          
     758 files ignored.

github.com/AlDanial/cloc v 1.98  T=3.32 s (333.4 files/s, 251886.4 lines/s)
--------------------------------------------------------------------------------
Language                      files          blank        comment           code
--------------------------------------------------------------------------------
PO File                         308         117175         136711         318639
C                               320          32531          10007         140255
Python                           85           5185           6486          21660
C/C++ Header                    224           3129           5474          10668
Glade                             2              0            109           7446
Markdown                         25           1312              0           5102
SWIG                              8            626            146           3691
Qt                                1              0              2           2415
make                             62            587            140           2125
yacc                              2             76             75           1418
Bourne Shell                     16            111            214            799
Bourne Again Shell                8            113            121            620
XML                               7             66             34            570
lex                               3             39             69            441
Text                             31            110              0            339
YAML                              6             40             32            258
--------------------------------------------------------------------------------
SUM:                           1108         161100         159620         516446
--------------------------------------------------------------------------------
[root@openeuler6 ~/rpmbuild/BUILD]# 
[root@openeuler6 ~]# rpm -ql policycoreutils-python-utils |xargs -i file {}
/etc/dbus-1/system.d/org.selinux.conf: XML 1.0 document, ASCII text
/etc/ima/digest_lists.tlv/0-metadata_list-compact_tlv-policycoreutils-python-utils-3.5-4.oe2403sp2.noarch: data
/etc/ima/digest_lists/0-metadata_list-compact-policycoreutils-python-utils-3.5-4.oe2403sp2.noarch: data
/usr/bin/audit2allow: Python script, ASCII text executable
/usr/bin/audit2why: symbolic link to audit2allow
/usr/bin/chcat: Python script, ASCII text executable
/usr/sbin/semanage: Python script, ASCII text executable, with very long lines (372)
/usr/share/bash-completion/completions/semanage: ASCII text
```

##  2. <a name='-1'></a>基本使用

###  2.1. <a name='semanage'></a>semanage

`semanage` 是 Linux 系统中用于管理 SELinux（Security-Enhanced Linux）策略的核心工具。与 `chcon` 命令仅能临时修改文件上下文不同，`semanage` 的修改是**永久性**的，它会将规则写入 SELinux 策略数据库，即使系统重启或文件重新标记，规则依然有效。

以下是 `semanage` 命令的主要用法及详细示例。

🔧 常用选项

在使用子命令时，通常会配合以下选项：
*   `-a`: 添加一条新规则。
*   `-d`: 删除一条规则。
*   `-m`: 修改一条现有规则。
*   `-l`: 列出当前所有规则。
*   `-t`: 指定 SELinux 类型（Type）。
*   `-p`: 指定协议（如 tcp, udp），主要用于端口管理。



 📁 管理文件上下文 (fcontext)

这是 `semanage` 最常用的功能，用于定义文件或目录的默认安全上下文。当你需要让服务（如 Apache、Nginx）访问非标准目录时，就需要用到它。

**基本语法：**
`semanage fcontext -a -t <类型> "<文件路径正则表达式>"`

**示例：让 Apache 访问自定义网站目录**
假设你的网站文件存放在 `/mywebsite`，但 Apache 默认只允许访问 `/var/www`。你需要将 `/mywebsite` 的上下文类型设置为 `httpd_sys_content_t`。

1.  **添加规则**：
2.  
```bash
sudo semanage fcontext -a -t httpd_sys_content_t "/mywebsite(/.*)?"
```
这里的 `(/.*)?` 是一个正则表达式，表示该规则不仅适用于 `/mywebsite` 目录本身，也适用于其下的所有文件和子目录。

1.  **使规则生效**：
    `semanage` 只是修改了策略库，要让更改立即应用到文件系统上，必须配合 `restorecon` 命令。
    ```bash
    sudo restorecon -Rv /mywebsite
    ```
    执行后，`/mywebsite` 目录及其内容的 SELinux 上下文就会被更新，Apache 服务便可以正常访问了。

---

 🌐 管理网络端口 (port)

当服务需要使用非标准端口时，必须使用 `semanage port` 将该端口添加到 SELinux 允许的类型中。

**基本语法：**
`semanage port -a -t <类型> -p <协议> <端口号>`

**示例：为 Apache 添加 8080 端口**
如果你想让 Apache 监听 8080 端口，但 SELinux 默认只允许 80 和 443 等端口，你需要执行：

1.  **添加端口规则**：
    ```bash
    sudo semanage port -a -t http_port_t -p tcp 8080
    ```
    这条命令将 TCP 协议的 8080 端口标记为 `http_port_t` 类型，允许 HTTP 服务使用。

2.  **查看端口规则**：
    你可以使用以下命令来验证规则是否添加成功：
    ```bash
    sudo semanage port -l | grep http_port_t
    ```
    输出会显示 `http_port_t` 类型现在包含了 80, 443, 以及你刚添加的 8080 端口。

---

⚙️ 管理布尔值 (boolean)

SELinux 布尔值是一种简化的策略开关，允许管理员在不修改底层策略的情况下，灵活地开启或关闭某些特定的访问权限。

**基本语法：**
`semanage boolean -m --on <布尔值名称>`

**示例：允许 Apache 访问网络**
默认情况下，SELinux 会阻止 Apache 进程发起网络连接（例如连接数据库或访问外部 API）。你可以通过布尔值来允许这种行为。

1.  **开启布尔值**：
    ```bash
    sudo semanage boolean -m --on httpd_can_network_connect
    ```
    这条命令会立即生效，允许 Apache 进行网络连接。

2.  **查看布尔值状态**：
    可以使用 `getsebool` 命令来查看布尔值的当前状态。
    ```bash
    getsebool httpd_can_network_connect
    ```
    输出 `httpd_can_network_connect --> on` 表示已成功开启。

---

👤 管理用户登录映射 (login)

此功能用于将 Linux 系统用户映射到特定的 SELinux 用户，从而限制该用户登录后的 SELinux 权限范围。

**基本语法：**
`semanage login -a -s <SELinux用户> <Linux用户>`

**示例：限制普通用户权限**
将系统用户 `dev_user` 映射到权限受限的 SELinux 用户 `user_u`。

```bash
sudo semanage login -a -s user_u dev_user
```
执行后，当 `dev_user` 登录系统时，其 SELinux 上下文将被限制在 `user_u` 的权限范围内，无法执行需要更高权限的操作。

---

 📦 管理策略模块 (module)

`semanage module` 用于管理 SELinux 策略模块，可以列出、安装或禁用模块。

**常用操作：**

*   **列出所有模块**：
    ```bash
    sudo semanage module -l
    ```
*   **禁用一个模块**：
    如果你想临时禁用某个自定义模块 `mymodule`，可以执行：
    ```bash
    sudo semanage module -D mymodule
    ```
    禁用后，该模块定义的所有规则将不再生效。





###  2.2. <a name='audit2allow'></a>audit2allow





###  2.3. <a name='audit2why'></a>audit2why




###  2.4. <a name='chcat'></a>chcat





##  3. <a name='-1'></a>软件源码





---


