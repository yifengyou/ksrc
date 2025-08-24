#!/usr/bin/python3

import os
import fcntl
from ctypes import *

def get_file_blocks(file_path):
    """
    获取文件在磁盘上的物理块信息

    参数:
        file_path: 文件路径

    返回:
        包含物理块号的列表，如 [36352, 36353, ...]
    """

    # Linux FIEMAP ioctl 常量定义
    FIEMAP = 0xC020660B
    FIEMAP_MAX_OFFSET = 0xFFFFFFFFFFFFFFFF
    FIEMAP_FLAG_SYNC = 0x00000001
    FIEMAP_FLAG_XATTR = 0x00000002
    FIEMAP_FLAG_DEVICE_ORDER = 0x40000000

    # 定义FIEMAP extent结构体
    class fiemap_extent(Structure):
        _fields_ = [
                ("fe_logical", c_uint64),    # 文件中的逻辑偏移 (bytes)
                ("fe_physical", c_uint64),   # 磁盘上的物理偏移 (bytes)
                ("fe_length", c_uint64),     # 长度 (bytes)
                ("fe_reserved64", c_uint64 * 2),
                ("fe_flags", c_uint32),      # 标志位
                ("fe_reserved", c_uint32 * 3),
                ]

    # 定义FIEMAP结构体
    class fiemap(Structure):
        _fields_ = [
                ("fm_start", c_uint64),          # 起始逻辑偏移 (bytes)
                ("fm_length", c_uint64),         # 映射长度 (bytes)
                ("fm_flags", c_uint32),         # 标志位
                ("fm_mapped_extents", c_uint32), # 已映射的extent数量
                ("fm_extent_count", c_uint32),   # 分配的extent数量
                ("fm_reserved", c_uint32),
                ]

    try:
        # 获取文件大小
        file_size = os.path.getsize(file_path)
        if file_size == 0:
            return []

        fd = os.open(file_path, os.O_RDONLY)

        # 初始化fiemap结构
        fm = fiemap()
        fm.fm_start = 0                     # 从文件开头开始
        fm.fm_length = FIEMAP_MAX_OFFSET    # 映射整个文件
        fm.fm_flags = 0                     # 无特殊标志
        fm.fm_extent_count = 0              # 初始时不请求extent
        fm.fm_mapped_extents = 0           # 初始化为0

        # 第一次ioctl调用 - 获取需要的extent数量
        fcntl.ioctl(fd, FIEMAP, fm)

        if fm.fm_mapped_extents == 0:
            return []

        # 准备足够大的缓冲区来保存所有extent
        buf_size = sizeof(fiemap) + fm.fm_mapped_extents * sizeof(fiemap_extent)
        buf = create_string_buffer(buf_size)

        # 将fiemap结构复制到缓冲区开头
        memmove(buf, addressof(fm), sizeof(fiemap))

        # 设置实际要获取的extent数量
        fm_ptr = cast(buf, POINTER(fiemap))
        fm_ptr.contents.fm_extent_count = fm.fm_mapped_extents

        # 第二次ioctl调用 - 获取实际的extent数据
        fcntl.ioctl(fd, FIEMAP, fm_ptr.contents)

        blocks = set()  # 使用集合避免重复块

        # 获取块大小 (通常为4096字节)
        stat = os.statvfs(file_path)
        block_size = stat.f_bsize

        # 处理每个extent
        for i in range(fm_ptr.contents.fm_mapped_extents):
            # 获取extent结构
            ext_offset = sizeof(fiemap) + i * sizeof(fiemap_extent)
            ext = fiemap_extent.from_buffer(buf, ext_offset)

            # 计算物理块号 (物理偏移/块大小)
            start_block = ext.fe_physical // block_size
            end_block = (ext.fe_physical + ext.fe_length - 1) // block_size

            # 添加所有块到集合中
            blocks.update(range(start_block, end_block + 1))

        # 转换为排序后的列表
        return sorted(blocks)

    except (IOError, OSError) as e:
        print(f"Error getting file blocks: {e}")
        return []
    finally:
        if 'fd' in locals():
            os.close(fd)

if __name__ == "__main__":
    import sys

    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file_path>")
        sys.exit(1)

    file_path = sys.argv[1]
    blocks = get_file_blocks(file_path)

    print(f"Physical blocks for file '{file_path}':")
    for block in blocks:
        print(block)


