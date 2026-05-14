# SE Linux基本原理

| 上下文部分 | 示例值 | 对应的安全模型 | 说明 |
| :--- | :--- | :--- | :--- |
| User | `unconfined_u` | Identity | 标识是谁（映射自 Linux 用户） |
| Role | `unconfined_r` | RBAC | 决定用户可以进入哪些域 |
| Type | `unconfined_t` | TE | 核心：决定进程能访问哪些资源 |
| Level | `s0:c0.c1023` | MLS / MCS | 决定敏感度和类别范围（可选） |


TE (Type Enforcement)：绝对的核心与基石。
如果没有 TE，SELinux 就失去了 90% 以上的功能。
它是所有策略规则的基础，决定了“谁能访问什么”。
即使你禁用了 RBAC、MLS 和 MCS，只要 TE 在工作，SELinux 就在进行强制访问控制。

RBAC (Role-Based Access Control)：必要的桥梁。
它连接了“用户”和“域（TE 的一部分）”。
在默认策略中，它的作用比较固定（通常是一对一映射），但在高安全场景下非常关键。
没有 RBAC，SELinux 用户就无法合法地进入特定的进程域。

MLS / MCS (Multi-Level / Multi-Category Security)：可选的增强层。
它们是建立在 TE 和 RBAC 之上的额外检查层。

MCS 是现代 Linux（如 RHEL/CentOS/OpenEuler）的默认配置（主要用于容器隔离和多租户）。

MLS 是可选配置，通常用于极高安全需求的军事/政府场景，默认是关闭的。
如果系统未启用 MLS/MCS，安全上下文中就没有 level 部分（即只有 user:role:type），此时 SELinux 依然正常工作。


---

总结：

SELinux 就像是一个超级严格的保安，它的核心原则是：“除非我明确允许，否则一切禁止”

SELinux = TE (核心) + RBAC (桥梁) + MLS/MCS (增强/隔离)

* TE 解决了“进程能干什么”的问题。
* RBAC 解决了“谁能变成那个进程”的问题。
* MLS/MCS 解决了“数据分级和隔离”的问题。



---


## SELinux状态管理

![](./images/2227051336300.png)

![](./images/2668595601700.png)


## SELinux标签

![](./images/2715511802500.png)

![](./images/3041454576400.png)

```用户:角色:类型:级别```

![](./images/3902136194700.png)


## SELinux标签存储在inode中

![](./images/2874477209800.png)


## SELinux用户管理

* 独立于系统用于，映射

![](./images/4113166759599.png)

![](./images/5324727140100.png)

![](./images/5422286699300.png)

![](./images/6205252032600.png)

获取linux用户与selinux用户的映射关系

![](./images/6266739123200.png)

![](./images/6360575345300.png)

![](./images/6398701732000.png)





---