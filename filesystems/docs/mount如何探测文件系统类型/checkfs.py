import struct
import sys
import os

# 定义常见文件系统的魔数（偏移量，魔数值，文件系统名称）
# 注意：这里的偏移量是针对分区起始位置而言的
FILESYSTEM_SIGNATURES = [
        (510,  b'\x55\xAA', 'vfat (FAT12/16/32)'), # FAT系列引导扇区结束标志
        (1080, b'\x53\xEF', 'ext2/ext3/ext4'),  # Ext2/3/4 的魔数在超级块偏移 56 处 (1024+56=1080)
        (0,    b'XFSB',     'XFS'),             # XFS 的魔数在头部
        (64,   b'\x88\x12\x05\x2003', 'F2FS'),  # F2FS 的魔数
        (56,   b'\x4d\x5a', 'FAT/NTFS (MBR)'),  # 简单的 DOS/Windows 分区识别
        (1024, b'\x48\x2b', 'HFS'),             # HFS (Mac)
        (0,    b'\x00\x00\x00\x00\x00\x00\x00\x00', 'Empty/Unknown')
        ]

def check_filesystem(device_path):
    """
    读取块设备或文件，解析其文件系统类型
    """
    if not os.path.exists(device_path):
        print(f"❌ 错误: 找不到设备或文件 '{device_path}'")
        return

    try:
        # 以二进制只读模式打开设备/文件
        with open(device_path, 'rb') as f:
            print(f"🔍 正在扫描: {device_path}")

            # 为了匹配 Ext 系列，我们需要读取至少 1080 字节的数据
            # 大多数超级块信息都在前 2KB 范围内
            header = f.read(2048)
            if len(header) < 1080:
                print("❌ 读取的数据不足，无法识别。")
                return

            # 遍历我们定义的魔数列表进行比对
            for offset, magic, fs_name in FILESYSTEM_SIGNATURES:
                # 提取对应偏移量的字节数据
                # 比如 Ext4，提取 offset=1080 开始的 2 个字节
                magic_len = len(magic)
                if offset + magic_len <= len(header):
                    signature = header[offset:offset + magic_len]
                    if signature == magic:
                        print(f"✅ 识别成功! 文件系统类型极有可能是: 【{fs_name}】")
                        return fs_name

            # 如果循环结束都没有匹配上
            print("⚠️  未识别出已知的常见文件系统魔数。")
            # 打印前16个字节的十六进制数据，方便进一步排查
            print(f"   头部前16字节(Hex): {header[:16].hex(' ').upper()}")
            return None

    except PermissionError:
        print(f"❌ 权限不足! 请使用 sudo 运行此脚本 (例如: sudo python3 {sys.argv[0]} {device_path})")
    except Exception as e:
        print(f"❌ 发生未知错误: {e}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("用法: python3 checkfs.py <设备路径或镜像文件>")
        print("示例: sudo python3 checkfs.py /dev/sdb1")
        print("示例: python3 checkfs.py disk_image.img")
    else:
        target = sys.argv[1]
        check_filesystem(target)

