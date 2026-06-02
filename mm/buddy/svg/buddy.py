# ！/usr/bin/env python3

import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

# 设置画布
fig, ax = plt.subplots(figsize=(20, 5))

# 假设我们有一个 10 阶 buddy 系统，总大小为 2^10 = 1024 单位
total_size = 2 ** 10
block_size = 1  # 最小块大小

# 模拟 buddy 系统的分配状态（这里简化为全未分配）
# 可以根据实际需求修改 blocks 列表来表示已分配或空闲块
blocks = [(i * block_size, block_size) for i in range(total_size)]

# 绘制每个块
for start, size in blocks:
    rect = Rectangle((start, 0), size, 1, linewidth=0.2, edgecolor='black', facecolor='white')
    ax.add_patch(rect)

# 设置坐标轴范围
ax.set_xlim(0, total_size)
ax.set_ylim(0, 1)
ax.set_aspect('equal')

# 隐藏坐标轴
plt.axis('off')

# 保存为 SVG 文件
plt.savefig("buddy_system.svg", format="svg", bbox_inches='tight')
plt.show()
