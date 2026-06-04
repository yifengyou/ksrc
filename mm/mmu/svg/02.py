#!/usr/bin/env python3

import matplotlib.pyplot as plt


def mmu_4level(output="mmu_4level.svg"):
    n_boxes = 64
    box_width = 1.0
    total_width = n_boxes * box_width

    # 调高高度，放下两层内容
    fig_width = max(12, n_boxes * 0.18)
    fig, ax = plt.subplots(figsize=(fig_width, 6))

    ax.set_xlim(0, total_width)
    ax.set_ylim(-1.8, 1.2)  # 下方留出空间画分段
    ax.axis('off')

    # ====================== 上层：64个小格子（不变） ======================
    for i in range(n_boxes):
        x = i * box_width
        rect = plt.Rectangle((x, 0.4), box_width, 0.5,
                             linewidth=1,
                             edgecolor='black',
                             facecolor='none')
        ax.add_patch(rect)
        ax.text(x + box_width / 2, 0.65, f"{63 - i}",
                ha='center', va='center',
                fontsize=5, family='monospace')

    # ====================== 下层：分段框（另起一层，不覆盖） ======================
    # 64位分段：16 + 9 + 9 + 9 + 9 + 12
    sections = [
        (0, 16, "Reserved", "#f0f0f0"),  # 左1：16位
        (16, 9, "PGD Index", "#ffe4e1"),  # 9位
        (25, 9, "PUD Index", "#e1f5fe"),  # 9位
        (34, 9, "PMD Index", "#f3e5f5"),  # 9位
        (43, 9, "PTE Index", "#fff9c4"),  # 9位
        (52, 12, "Page Offset", "#e8f5e9")  # 右1：12位
    ]

    # 下方分段框 Y 位置
    section_y = -0.2
    section_height = 0.6

    for start_x, length, name, color in sections:
        x = start_x * box_width
        w = length * box_width

        # 绘制分段大框
        rect = plt.Rectangle((x, section_y), w, section_height,
                             linewidth=1.0,
                             edgecolor='black',
                             facecolor=color, alpha=0.8)
        ax.add_patch(rect)

        # 分段名称
        ax.text(x + w / 2, section_y + section_height / 2,
                f"{name}\n{length}bit({2 ** length})",
                ha='center', va='center',
                fontsize=6, fontweight='bold')

    # index
    sections = [
        (0, 16, "N/A", "#f0f0f0"),  # 左1：16位
        (16, 9, "PGD Index", "#ffe4e1"),  # 9位
        (25, 9, "PUD Index", "#e1f5fe"),  # 9位
        (34, 9, "PMD Index", "#f3e5f5"),  # 9位
        (43, 9, "PTE Index", "#fff9c4"),  # 9位
        (52, 12, "Page Offset", "#e8f5e9")  # 右1：12位
    ]
    section_y = -1.5
    section_height = 1

    for start_x, length, name, color in sections:
        x = start_x * box_width
        w = (length * box_width) * 0.8
        if x == 0:
            # 绘制索引页面
            rect = plt.Rectangle((x, section_y), w, 0.3,
                                 linewidth=1.0,
                                 edgecolor='black',
                                 facecolor=color, alpha=0.8)
            ax.add_patch(rect)
            continue
        # 绘制索引页面
        rect = plt.Rectangle((x, section_y), w, section_height,
                             linewidth=1.0,
                             edgecolor='black',
                             facecolor=color, alpha=0.8)
        ax.add_patch(rect)
        # 绘制目标索引项
        rect = plt.Rectangle((x, section_y + 0.3), w, 0.1,
                             linewidth=1.0,
                             edgecolor='black',
                             facecolor='gray', alpha=0.8)
        ax.add_patch(rect)

        # 索引
        ax.text(x + w / 2, section_y + section_height / 2,
                f"{name}\n{length}bit({2 ** length}) {x}/{section_y}",
                ha='center', va='center',
                fontsize=6, fontweight='bold')# 分段名称
        # 索引项
        ax.text(x + w / 2, section_y + 0.35,
                f"0XFFFFFFFFFFFFFFFF",
                ha='center', va='center',
                fontsize=6, fontweight='bold')
    # ====================== 标题 & 版权 ======================
    plt.title("MMU 4-Level Virtual Address (64-bit)", fontsize=10, pad=10)

    copyright_text = "© 2026–2066 The Ksrc Project"
    ax.text(total_width / 2, -1.6, copyright_text,
            ha='center', va='top', fontsize=5, color='gray', style='oblique')

    # 保存
    plt.savefig(output, format='svg', bbox_inches='tight', pad_inches=0.1, transparent=True)
    plt.close()
    print(f"{output} saved successfully")


if __name__ == "__main__":
    mmu_4level()
