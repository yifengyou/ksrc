#!/bin/bash
set -x
#set -e

# 参数配置
TARGET_ARCH="amd64"          # 目标架构
DISTRO="bionic"              # Ubuntu 18.04 代号
MIRROR="http://mirrors.aliyun.com/ubuntu"  # 阿里云镜像源
ROOTFS_DIR="./ubuntu-rootfs" # 根文件系统目录
OUTPUT_IMG="initrd.img"      # 输出镜像名称

# 安装依赖工具
echo "[*] 安装依赖..."
apt-get update
apt-get install -y debootstrap qemu-user-static cpio gzip

# 创建根文件系统目录
echo "[*] 创建根文件系统目录..."
mkdir -p "${ROOTFS_DIR}"

# 使用 debootstrap 构建基础系统
echo "[*] 开始构建根文件系统 (Ubuntu ${DISTRO})..."
debootstrap --arch="${TARGET_ARCH}" \
                 --components=main,universe \
                 --include=busybox,sudo,bash,strace,pciutils \
                 "${DISTRO}" \
                 "${ROOTFS_DIR}" \
                 "${MIRROR}"

# 配置必要设备节点
echo "[*] 创建设备节点..."
mkdir -p "${ROOTFS_DIR}/dev"
mkdir -p "${ROOTFS_DIR}/dev" "${ROOTFS_DIR}/dev/pts" "${ROOTFS_DIR}/proc" "${ROOTFS_DIR}/sys"
#mknod -m 622 "${ROOTFS_DIR}/dev/console" c 5 1
#mknod -m 666 "${ROOTFS_DIR}/dev/null"    c 1 3
#mknod -m 666 "${ROOTFS_DIR}/dev/tty"     c 5 0
#mknod -m 666 "${ROOTFS_DIR}/dev/tty0"    c 4 0

# 编写初始化脚本
echo "[*] 配置初始化脚本..."
tee "${ROOTFS_DIR}/init" > /dev/null <<'EOF'
#!/bin/busybox sh

# 设置终端环境变量

export HOME=/root
export LC_ALL=C
export LANG=C.UTF-8
export TERM=xterm-256color
export HOSTNAME=kdev

# 挂载虚拟文件系统
mount -t proc  proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp

exec 0</dev/console
exec 1>/dev/console
exec 2>/dev/console

chmod 0622 /dev/console

# 启动交互式shell
echo -e "\nWelcome to Ubuntu 18.04 in initrd!"
exec /bin/bash
EOF

# 设置权限
chmod +x "${ROOTFS_DIR}/init"

# 打包为 initrd.img
echo "[*] 打包为 initrd.img..."
cd "${ROOTFS_DIR}"
find . | cpio -H newc -o | gzip > "../${OUTPUT_IMG}"
cd ..

echo "[✓] 构建完成！输出文件: ${OUTPUT_IMG}"
