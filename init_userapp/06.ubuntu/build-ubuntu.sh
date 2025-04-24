#!/bin/bash

# 将 Docker 镜像转换为 initrd.img
# 示例: ./docker-to-initrd.sh ubuntu:18.04

# set -e  # 任何命令失败则退出脚本

# 检查参数
if [ $# -ne 1 ]; then
  echo "用法: $0 <docker镜像名:标签>"
  exit 1
fi

WORKDIR=`pwd`
DOCKER_IMAGE="$1"
TMP_DIR="$(mktemp -d)"
ROOTFS_DIR="$TMP_DIR/rootfs"
OUTPUT_FILE="initrd.img"

# 清理函数
cleanup() {
  echo "清理临时目录..."
  #rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# 创建目录结构
mkdir -p "$ROOTFS_DIR"/{bin,dev,etc,lib,proc,sys,usr/lib}

# 从 Docker 镜像导出文件系统
echo "导出 Docker 镜像: $DOCKER_IMAGE..."
DOCKER_CONTAINER=$(docker create "$DOCKER_IMAGE" /bin/true)
docker export "$DOCKER_CONTAINER" | tar -x -C "$ROOTFS_DIR"
docker rm "$DOCKER_CONTAINER" >/dev/null

# 创建设备节点 (需 root 权限)
echo "创建设备节点..."
mknod -m 622 "$ROOTFS_DIR/dev/console" c 5 1
mknod -m 666 "$ROOTFS_DIR/dev/null" c 1 3

# 添加初始化脚本
echo "创建 init 脚本..."
tee "$ROOTFS_DIR/init" >/dev/null <<EOF
#!/bin/sh

echo "yyf loading..."

# 初始化流程
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# 动态库路径（防止缺失库错误）
export LD_LIBRARY_PATH="/lib:/usr/lib:/lib64"

# 启动 Shell（或自定义命令）


ulimit -c unlimited

exec /bin/sh
EOF

# 设置 init 可执行权限
chmod +x "$ROOTFS_DIR/init"

# 修复动态库依赖（可选）
echo "修复动态库..."
for BIN in /bin/sh /bin/mount /bin/echo /bin/exec; do
  if [ -f "$ROOTFS_DIR$BIN" ]; then
    ldd "$ROOTFS_DIR$BIN" | grep "=>" | awk '{print $3}' \
      | xargs -I '{}' cp --parents '{}' "$ROOTFS_DIR"
  fi
done

# 生成 initrd.img
echo "构建 $OUTPUT_FILE..."
cd "$ROOTFS_DIR"
find . | cpio -H newc -o 2>/dev/null | gzip -9 > "$TMP_DIR/$OUTPUT_FILE"

# 移动最终文件
mv "$TMP_DIR/$OUTPUT_FILE" ${WORKDIR}/
echo "生成成功! 输出文件: $(realpath "$OUTPUT_FILE")"
