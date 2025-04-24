# cgroup2

## 简介


cgroup v2 是 Linux cgroup API 的新版本，它在 2016 年发布，并在 2019 年被标记为稳定1。cgroup v2 对 cgroup v1 进行了多项改进，

例如：

1. API 中单个统一的层次结构设计
2. 更安全的子树委派给容器
3. 更新的功能特性，例如压力阻塞信息（Pressure Stall Information，PSI）
4. 跨多个资源的增强资源分配管理和隔离
3. 统一核算不同类型的内存分配（网络内存、内核内存等）

cgroup v2 已经被许多 Linux 发行版和容器运行时支持，并且已经在一些生产环境中使用。例如：

1. Container-Optimized OS（从 M97 开始）
2. Ubuntu（从 21.10 开始，推荐 22.04+）
3. Debian GNU/Linux（从 Debian 11 Bullseye 开始）
4. Fedora（从 31 开始）
5. Arch Linux（从 2021 年 4 月开始）
6. RHEL 和类似 RHEL 的发行版（从 9 开始）
7. containerd（从 v1.4 开始）
8. cri-o（从 v1.20 开始）

因此，cgroup v2 是一个稳定且适合线上环境使用的技术，但是在使用之前，您需要确保您的操作系统、内核和容器运行时都支持 cgroup v2，并且您的应用程序能够正确地适配 cgroup v2 的 API 和功能。
