#!/usr/bin/env python3

# 导入matplotlib绘图库，pyplot是核心绘图模块
import matplotlib.pyplot as plt

def mmu_4level(output="mmu_4level.svg"):
    # ====================== 基础画布参数定义 ======================
    # 总共有64个小格子，对应64位虚拟地址
    n_boxes = 64
    # 每个小格子的宽度（坐标单位）
    box_width = 1.0
    # 整个图表总宽度 = 64格 × 每格宽度
    total_width = n_boxes * box_width

    # 画布宽度：最小12，否则按每格0.18比例计算，保证不挤
    fig_width = max(12, n_boxes * 0.18)
    # 创建画布：fig是整张图，ax是绘图区域，尺寸(宽, 高)
    fig, ax = plt.subplots(figsize=(fig_width, 7.5))

    # 设置X轴范围：0 到 总宽度（64格）
    ax.set_xlim(0, total_width)
    # 设置Y轴范围：-3 到 1.5，上下留出布局空间
    ax.set_ylim(-3.0, 1.5)
    # 关闭坐标轴显示（干净图表）
    ax.axis('off')

    # ====================== 64位地址分段固定定义 ======================
    # 格式：(起始bit, 长度bit, 分段名, 填充色)
    # 位编号：63→0，所以start_bit越大，实际在图中越靠右
    addr_segments = [
        (0, 16, "Reserved", "#f0f0f0"),     # bit 0~15：保留位
        (16, 9, "PGD Index", "#ffe4e1"),    # bit16~24：PGD索引
        (25, 9, "PUD Index", "#e1f5fe"),    # bit25~33：PUD索引
        (34, 9, "PMD Index", "#f3e5f5"),    # bit34~42：PMD索引
        (43, 9, "PTE Index", "#fff9c4"),    # bit43~51：PTE索引
        (52, 12, "Page Offset", "#e8f5e9")  # bit52~63：页内偏移
    ]

    # 页表块只需要后5段（跳过Reserved）
    # block_indices = addr_segments[1]到[5]的索引
    block_indices = [1, 2, 3, 4, 5]
    # 页表层级名称
    block_names = ["PGD", "PUD", "PMD", "PTE", "Page"]
    # 从地址段中取出对应颜色
    block_colors = [addr_segments[i][3] for i in block_indices]

    # ====================== 顶层：64位bit编号（最上方一行） ======================
    # Y坐标固定在 1.0~1.4 之间
    for i in range(n_boxes):
        # 每个格子的X起始坐标 = 第i格 × 每格宽度
        x = i * box_width
        # 画矩形：(x起始, y起始), 宽度, 高度
        rect = plt.Rectangle((x, 1.0), box_width, 0.4,
                             linewidth=0.8,    # 边框粗细
                             edgecolor='black',# 边框黑色
                             facecolor='none') # 无填充
        ax.add_patch(rect)

        # 在格子中心写bit编号：63,62,...0
        ax.text(x + box_width / 2, 1.2,  # 文字中心坐标(x居中, y=1.2)
                f"{63 - i}",            # 数字：63-i
                ha='center', va='center',
                fontsize=4.5, family='monospace')

    # ====================== 第二层：地址分段彩色大区块 ======================
    # 分段区域Y起始位置
    y_seg = 0.3
    # 分段区域高度
    h_seg = 0.7

    for start_bit, length, name, color in addr_segments:
        # X起始 = 起始bit × 每格宽度
        x = start_bit * box_width
        # 区块宽度 = bit长度 × 每格宽度
        w = length * box_width

        # 画彩色分段矩形
        rect = plt.Rectangle((x, y_seg), w, h_seg,
                             linewidth=1.0,
                             edgecolor='black',
                             facecolor=color, alpha=0.8)
        ax.add_patch(rect)

        # 中心文字：名称 + bit数 + 项数
        ax.text(x + w / 2, y_seg + h_seg / 2,
                f"{name}\n{length}bit({2 ** length})",
                ha='center', va='center',
                fontsize=6, fontweight='bold')

    # ====================== 第三层：四级页表结构方块（下半部分） ======================
    # 页表块顶部Y坐标
    block_y_top = -2.0
    # 页表块总高度
    block_height = 1.8
    # 页表块宽度缩放系数（比地址段窄一点，更美观）
    block_width_factor = 0.75

    # 存储所有页表块信息，用于后续画箭头
    blocks = []

    for idx, name, color in zip(block_indices, block_names, block_colors):
        # 取出对应地址段的：起始bit、长度
        start_bit, length, _, _ = addr_segments[idx]
        # 页表块宽度
        w = length * box_width * block_width_factor
        # X坐标 = 对应地址段的起始bit位置（对齐上方分段）
        x = start_bit * box_width

        # ---------- 画页表主体框 ----------
        rect_main = plt.Rectangle((x, block_y_top), w, block_height,
                                  linewidth=1.0,
                                  edgecolor='black',
                                  facecolor=color, alpha=0.6)
        ax.add_patch(rect_main)

        # ---------- 画灰色的表项行（中间横条） ----------
        # 灰色条Y起始 = 块顶部 + 0.3
        # 灰色条高度 = 0.2
        rect_entry = plt.Rectangle((x, block_y_top + 0.3), w, 0.2,
                                   linewidth=1.0,
                                   edgecolor='black',
                                   facecolor='gray', alpha=0.9)
        ax.add_patch(rect_entry)

        # ---------- 写页表名称 ----------
        ax.text(x + w / 2, block_y_top + block_height - 0.3,
                name, ha='center', va='bottom', fontsize=7, fontweight='bold')

        # ---------- 写灰色条内文字 ----------
        ax.text(x + w / 2, block_y_top + 0.4,
                "Entry 0xFFFFFFFFFFFFFFFF", ha='center', va='center',
                fontsize=5, color='white')

        # ---------- 记录块坐标，给箭头用 ----------
        # 地址段中心X坐标
        seg_center_x = start_bit * box_width + length * box_width / 2
        blocks.append({
            'x': x,                  # 块左X
            'w': w,                  # 块宽
            'center_x': x + w / 2,   # 块中心X
            'entry_y_top': block_y_top + 0.3,  # 灰色条目顶部Y
            'seg_center_x': seg_center_x      # 对应地址段中心X
        })

    # ====================== 绿色箭头：页表索引指向流程 ======================
    green_arrow_style = {
        'arrowstyle': '-|>',        # 箭头样式
        'color': 'green',          # 绿色
        'linewidth': 2.0,          # 粗细
        'connectionstyle': 'arc3,rad=0.2'  # 轻微弧线
    }

    # 画4个箭头：PGD→PUD → PMD → PTE → Page
    for i in range(4):
        # 箭头起点：地址段中心
        start_x = blocks[i]['center_x']
        start_y = blocks[i]['entry_y_top'] + 0.1
        # 箭头终点：下一级页表块中心
        # end_x = blocks[i + 1]['center_x']
        # end_y = blocks[i + 1]['entry_y_top']
        end_x = blocks[i + 1]['x'] + 1.0          # 左边界稍右移，避免贴边
        end_y = block_y_top

        # 绘制箭头
        ax.annotate("", xy=(end_x, end_y), xytext=(start_x, start_y),
                    arrowprops=green_arrow_style)

    # ====================== 红色对角线：索引范围框角连线 ======================
    red_line_style = {
        'color': 'red',
        'linewidth': 1.0,
        'linestyle': '-'
    }

    # 只画前4级索引：PGD、PUD、PMD、PTE
    for i in range(4):
        start_bit, length, _, _ = addr_segments[block_indices[i]]
        # 分段右上角X坐标
        x1 = (start_bit + length) * box_width
        # 分段左下角X坐标
        x0 = start_bit * box_width

        # 画红线：从分段右上角 → 页表块左下角
        ax.annotate("",
                    xy=(x0 + 3, block_y_top + block_height),  # 终点：页表块左下角
                    xytext=(x1 - 5, y_seg),                  # 起点：地址分段右上角
                    arrowprops=red_line_style)

    # ====================== 标题与版权 ======================
    plt.title("MMU 4-Level Virtual Address (64-bit)", fontsize=11, pad=15)
    copyright_text = "© 2026–2066 The Ksrc Project"
    # 版权文字放在底部中央：Y=-2.9
    ax.text(total_width / 2, -2.9, copyright_text,
            ha='center', va='top', fontsize=5, color='gray', style='oblique')

    # 保存SVG图片，紧凑布局
    plt.savefig(output, format='svg', bbox_inches='tight', pad_inches=0.1, transparent=True)
    plt.close()
    print(f"{output} saved successfully")

if __name__ == "__main__":
    mmu_4level()
