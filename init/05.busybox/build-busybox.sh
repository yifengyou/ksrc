#!/bin/bash

# 构建完整的基于 BusyBox 的 initramfs 脚本
# 输出文件: initrd.img

set -e

# -------------------- 配置参数 --------------------
BUSYBOX_VERSION="1.36.1"    # BusyBox 版本
OUTPUT_DIR="initramfs"      # 临时构建目录
OUTPUT_FILE="initrd.img"  # 最终输出文件名
STATIC_BUILD=true           # 是否静态编译 BusyBox
# --------------------------------------------------

# 清理旧文件
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# -------------------- 下载并编译 BusyBox --------------------
if [ ! -f "busybox-$BUSYBOX_VERSION.tar.bz2" ]; then
  echo "下载 BusyBox v$BUSYBOX_VERSION..."
  wget -c https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2
fi

echo "解压 BusyBox..."
tar -xf busybox-$BUSYBOX_VERSION.tar.bz2

echo "编译 BusyBox (静态链接: $STATIC_BUILD)..."
cd busybox-$BUSYBOX_VERSION
make defconfig

if [ "$STATIC_BUILD" = true ]; then
  sed -i 's/^.*CONFIG_STATIC.*$/CONFIG_STATIC=y/' .config
fi

make defconfig
make -j$(nproc) LDFLAGS="-static"
cp busybox ../$OUTPUT_DIR/
cd ..

# -------------------- 构建 initramfs 目录结构 --------------------
cd "$OUTPUT_DIR"

# 创建标准目录
mkdir -p {bin,sbin,etc,proc,sys,dev,usr/{bin,sbin},lib}
chmod 755 dev

# 创建设备节点 (必要的基础设备)
sudo mknod -m 622 dev/console c 5 1  # 控制台设备
sudo mknod -m 666 dev/null c 1 3     # null设备

# 复制 BusyBox
cp ../busybox-$BUSYBOX_VERSION/busybox bin/
chmod +x bin/busybox

# 创建符号链接 (让 BusyBox 覆盖所有命令)
ln -s busybox bin/sh
ln -s ../bin/busybox sbin/init

# -------------------- 创建 init 脚本 --------------------
cat > init <<EOF
#!/bin/busybox sh

# 初始化系统
/bin/busybox --install -s  # 创建所有命令的符号链接

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo -e "\n\033[32mInitramfs 启动成功!\033[0m"
exec /bin/sh
EOF

chmod +x init

# -------------------- 生成 CPIO 归档 --------------------
echo "构建 initrd.img..."
find . -print0 | cpio --null -H newc -ov 2>/dev/null | gzip -9 > ../$OUTPUT_FILE

# -------------------- 清理和输出信息 --------------------
cd ..
echo -e "\n构建完成! 输出文件: \033[1m$OUTPUT_FILE\033[0m"
