#!/usr/bin/python3

import matplotlib.pyplot as plt


def plot_buddy_system_non_recursive(max_level=10):
    """
    使用循环绘制 Buddy 系统结构图，并在矩形中心添加标签。
    """
    fig, ax = plt.subplots(figsize=(300, 10))

    total_size = 2 ** max_level

    # 设置绘图范围
    ax.set_xlim(0, total_size)
    ax.set_ylim(0, max_level + 1)
    ax.set_aspect('equal')

    # 隐藏坐标轴
    ax.set_xticks([])
    ax.set_yticks([])


    # 循环绘制每一层
    for level in range(max_level + 1):
        # 计算当前层的块大小
        block_size = 2 ** level

        # 循环绘制当前层的每个块
        for i in range(total_size // block_size):
            start = i * block_size
            end = start + block_size

            # 绘制矩形
            rect = plt.Rectangle((start, level), width=block_size, height=1, linewidth=0.2, edgecolor='black', facecolor='none')
            ax.add_patch(rect)

            # 在矩形中心添加标签
            if start == end -1:
                label = f"Index: {start}\nSize: {2**level}*4K\nMergeto:{start ^ (1 << level)}"
            else:
                label = f"Index: {start}-{end - 1}\nSize: {2**level}*4K\nMergeto:{start} ^ (1 << {level})={start ^ (1 << level)}"
            ax.text(start + block_size / 2, level + 0.5, label, ha='center', va='center', fontsize=2, color='black')

    # 添加层标签
    for i in range(max_level + 1):
        ax.text(-1, i + 0.5, f'Level {i:02}', va='center', ha='right', fontsize=12)

    plt.title('Buddy System (1024 Pages, 10 Levels)', fontsize=14)

    # 保存为 SVG 文件
    plt.savefig('buddy_system.svg', format='svg', bbox_inches=None)


if __name__ == "__main__":
    # 绘制 Buddy 系统
    plot_buddy_system_non_recursive()

