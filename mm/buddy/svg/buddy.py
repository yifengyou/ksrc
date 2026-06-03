#!/usr/bin/env python3

import matplotlib.pyplot as plt


def plot_buddy_system_non_recursive(max_level=10, output="buddy.svg"):
    """
    使用循环绘制 Buddy 系统结构图，并在矩形中心添加标签。
    """
    total_size = 2 ** max_level

    # 预定义每级的 figsize (width, height)
    FIGSIZE_CONFIG = {
        1: (8, 1.5),
        2: (8, 2),
        3: (12, 3),
        4: (24, 3),
        5: (48, 4),
        6: (96, 4),
        7: (192, 5),
        8: (384, 6),
        9: (800, 7),
        10: (1500, 8),
    }

    # 默认 fallback：如果 level 超出预设，用最大或动态估算
    if max_level in FIGSIZE_CONFIG:
        width, height = FIGSIZE_CONFIG[max_level]
    else:
        # 安全兜底：不超过 600 英寸
        width = min(600, total_size * 0.6)
        height = min(600, max_level + 4)

    print(f"width: {width} , height: {height}")
    fig, ax = plt.subplots(figsize=(width, height))

    # 设置绘图范围
    ax.set_xlim(0, total_size)
    ax.set_ylim(0, max_level + 1)
    ax.set_aspect('auto')

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
            rect = plt.Rectangle((start, level), width=block_size, height=1, linewidth=0.2, edgecolor='black',
                                 facecolor='none')
            ax.add_patch(rect)

            # 在矩形中心添加标签
            if start == end - 1:
                label = f"PFN: {start}\nSize: {2 ** level * 4}KB(2^{level}*4KB)\nBuddy:{start ^ (1 << level)}"
            else:
                label = f"PFN: {start}-{end - 1}\nSize: {2 ** level * 4}KB(2^{level}*4KB)\nBuddy:{start} ^ (1 << {level})={start ^ (1 << level)}"
            ax.text(start + block_size / 2, level + 0.5, label, ha='center', va='center', color='black', fontsize=8)

    # 添加层标签
    for i in range(max_level + 1):
        ax.text(-0.08, i + 0.5, f'Level {i:02}',
                va='center', ha='right', fontsize=12,
                clip_on=False)
    # 标题
    plt.title(f'Buddy System ({2 ** max_level} Pages, 4KB PerPage, {2 ** max_level * 4}KB Total, {max_level} Levels)',
              fontsize=14)

    # buddy规则
    rules = (
        "Buddy System Rules:\n"
        "1. Memory divided into blocks of size 2^n pages (4KB per page).\n"
        "2. n = block order; order 0 = 1 page, order 1 = 2 pages, etc.\n"
        "3. All blocks must be naturally aligned to their size.\n"
        "4. Two blocks are buddies: same order, contiguous, PFN differs in n-th bit.\n"
        "5. Buddy PFN = current PFN XOR (1 << block_order).\n"
        "6. Buddy relationship is mutual and exclusive for each block.\n"
        "7. Allocation: split larger free blocks recursively to fit request size.\n"
        "8. Only free blocks can be split; one order lower per split.\n"
        "9. Free: check buddy; if free, merge to higher order recursively.\n"
        "10. Merging only allowed between free buddy blocks of same order.\n"
        "11. Initial state: one free block of the maximum supported order.\n"
        "12. Allocated blocks keep their order; no partial changes.\n"
        "13. Eliminates external fragmentation; internal fragmentation may exist."
    )


    ax.text(
        0,
        -1,
        rules,
        ha='left',
        va='top',
        fontsize=10,
        fontfamily='monospace'
    )

    copyright_text = "© 2026–2066 The Ksrc Project"
    ax.text(total_size / 2, -0.5,  # 放在图表正下方居中
            copyright_text,
            ha='center', va='top', fontsize=10, color='black',
            style='oblique', clip_on=False)

    # 保存为 SVG 文件
    # plt.savefig(output, format='svg', bbox_inches=None)
    plt.savefig(output, format='svg', bbox_inches='tight', dpi=100)
    plt.close()
    print(f"{output} saved successfully")


if __name__ == "__main__":
    for i in range(1, 11):
        plot_buddy_system_non_recursive(max_level=i, output=f"buddy_power_{i}.svg")
