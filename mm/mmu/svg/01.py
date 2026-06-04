import matplotlib.pyplot as plt
import matplotlib.patches as patches

# 设置中文
plt.rcParams['font.sans-serif'] = ['SimHei']
plt.rcParams['axes.unicode_minus'] = False

fig, ax = plt.subplots(figsize=(16, 10))
ax.set_xlim(0, 12)
ax.set_ylim(0, 11)
ax.axis("off")
ax.set_title("Linux x86_64 五级页表(LA57)地址映射结构图｜5×9bit索引 + 12bit页内偏移", fontsize=15, weight='bold')

# 配色
col_pgd  = "#ffaaaa"
col_p4d  = "#ffdd99"
col_pud  = "#aaddff"
col_pmd  = "#bbffbb"
col_pte  = "#ffe0ff"
col_phy  = "#dddddd"

# ========== 1. 绘制虚拟地址分段(57位有效VA) ==========
y_va = 9.2
ax.text(0.3, y_va+0.3, "虚拟地址 VA(bit56~0)分段：", fontsize=12, weight='bold')
va_segs = [
    ("PGD idx\n9bit\nbit56~48",   0.3, 1.3),
    ("P4D idx\n9bit\nbit47~39",   1.4, 2.4),
    ("PUD idx\n9bit\nbit38~30",   2.5, 3.5),
    ("PMD idx\n9bit\nbit29~21",   3.6, 4.6),
    ("PTE idx\n9bit\nbit20~12",   4.7, 5.7),
    ("Offset\n12bit\nbit11~0",    5.8, 7.3),
]
for txt, x1, x2 in va_segs:
    r = patches.Rectangle((x1, y_va), x2-x1, 0.5, ec='k', fc='#f6f6f6')
    ax.add_patch(r)
    ax.text((x1+x2)/2, y_va+0.25, txt, ha='center', va='center', fontsize=8.5)

# ========== 2. 逐层绘制页表框 ==========
# PGD
pgd = patches.Rectangle((1.0,7.8),1.1,0.9,ec='k',fc=col_pgd)
ax.add_patch(pgd)
ax.text(1.55,8.3,"PGD\n页全局目录",ha='center',weight='bold')
ax.text(1.55,7.9,"CR3寄存器存PGD物理基址",ha='center',fontsize=8)

# P4D（五级独有层级）
p4d = patches.Rectangle((2.5,6.8),1.1,0.9,ec='k',fc=col_p4d)
ax.add_patch(p4d)
ax.text(3.05,7.3,"P4D\n页四级目录",ha='center',weight='bold')
ax.text(3.05,6.9,"五级分页新增层级",ha='center',fontsize=8)

# PUD
pud = patches.Rectangle((4.0,5.8),1.1,0.9,ec='k',fc=col_pud)
ax.add_patch(pud)
ax.text(4.55,6.3,"PUD\n页上级目录",ha='center',weight='bold')

# PMD
pmd = patches.Rectangle((5.5,4.8),1.1,0.9,ec='k',fc=col_pmd)
ax.add_patch(pmd)
ax.text(6.05,5.3,"PMD\n页中间目录",ha='center',weight='bold')

# PTE
pte = patches.Rectangle((7.0,3.8),1.1,0.9,ec='k',fc=col_pte)
ax.add_patch(pte)
ax.text(7.55,4.3,"PTE\n页表项",ha='center',weight='bold')

# 物理内存页
phy_page = patches.Rectangle((8.5,2.0),2.2,1.3,ec='k',fc=col_phy)
ax.add_patch(phy_page)
ax.text(9.6,2.9,"物理页帧(4KB)",ha='center',weight='bold')
ax.text(9.6,2.5,"Offset页内偏移拼接",ha='center',fontsize=9)

# ========== 3. 箭头连接映射关系 ==========
arrow_cfg = dict(arrowstyle="->",lw=1.6)
ax.annotate("",xy=(3.05,6.8),xytext=(1.55,7.8),arrowprops=arrow_cfg)
ax.annotate("",xy=(4.55,5.8),xytext=(3.05,6.8),arrowprops=arrow_cfg)
ax.annotate("",xy=(6.05,4.8),xytext=(4.55,5.8),arrowprops=arrow_cfg)
ax.annotate("",xy=(7.55,3.8),xytext=(6.05,4.8),arrowprops=arrow_cfg)
ax.annotate("",xy=(9.6,3.3), xytext=(7.55,3.8),arrowprops=arrow_cfg)

# ========== 4. 侧边详细文字说明 ==========
info_y_start = 1.6
info = [
    "【关键原理】",
    "1.CR3：CPU寄存器，存放当前进程PGD物理基地址(PA)，进程切换即换CR3",
    "2.每级idx：虚拟地址拆分出的数组下标，用来在当前页表中查表项",
    "3.各级页表项(PDE/PTE)：内容=下一级页表【物理地址】+权限标志位",
    "4.MMU硬件只通过物理地址访问内存，不能直接使用虚拟地址",
    "5.最终PA = PTE中物理页基址 <<12 | Offset(低12位)",
    "6.五级=5段9bit索引：5×9=45，+12偏移=57位VA；四级P4D被内核折叠",
    "7.单张页表固定4KB，每项8B，4096/8=512=2^9，故每级索引固定9bit"
]
for i,txt in enumerate(info):
    ax.text(0.3, info_y_start - i*0.22, txt, fontsize=9.8)

plt.tight_layout()
plt.savefig("mmu.svg", format='svg', bbox_inches='tight', dpi=100)
plt.close()
