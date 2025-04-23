# security



LSM（Linux Security Modules）是一个框架，允许Linux内核支持不同的计算机安全模型。LSM在Linux 2.6以后成为内核的标准部分。目前，官方内核中支持的安全模块有AppArmor、SELinux、Smack、TOMOYO Linux和Yama。¹ 这些模块都是基于LSM提供的钩子（hooks）来实现强制访问控制（Mandatory Access Control，MAC）的功能。²

下面是这些模块的简要介绍和比较：

- AppArmor：AppArmor是一种基于路径名的访问控制机制，它为每个程序定义了一个安全配置文件（profile），规定了该程序可以访问的文件、目录、网络端口等资源。AppArmor的优点是易于配置和使用，缺点是不能处理动态生成的文件名，以及对进程间通信的支持不完善。³
- SELinux：SELinux是一种基于标签的访问控制机制，它为每个对象（文件、进程、套接字等）和主体（用户、角色、域等）赋予一个安全上下文（security context），然后根据一组规则（policy）来判断是否允许主体对对象进行操作。SELinux的优点是功能强大和灵活，缺点是配置复杂和难以理解，以及对性能的影响较大。⁴
- Smack：Smack是一种简化的标签访问控制机制，它只为对象和主体赋予一个单一的字符串标签（label），然后根据一个访问矩阵（access matrix）来判断是否允许主体对对象进行操作。Smack的优点是简单和高效，缺点是功能有限和不易扩展，以及对用户空间的支持不完善。⁵
- TOMOYO Linux：TOMOYO Linux是一种基于域的访问控制机制，它将每个进程划分为一个域（domain），并为每个域定义了一组访问规则（access control list，ACL）。TOMOYO Linux的优点是易于学习和管理，缺点是对网络访问的控制较弱，以及对多用户环境的支持不完善。
- Yama：Yama是一种针对特定功能的访问控制机制，它主要用于限制进程间的调试（ptrace）和模块加载（module loading）等操作。Yama的优点是简洁和有效，缺点是功能单一和不通用，以及对用户空间的支持不完善。

这些模块之间的联系是，它们都是基于LSM框架实现的，都可以在内核编译时或运行时选择启用或禁用，都可以通过/proc或/sys文件系统进行配置和管理，都可以与传统的基于用户和组的访问控制（Discretionary Access Control，DAC）机制协同工作。

这些模块之间的区别是，它们采用了不同的访问控制模型和策略，适用于不同的场景和需求，具有不同的优缺点和性能影响，以及对用户空间的支持程度不同。

希望这些信息对您有所帮助。如果您还有其他问题，欢迎继续提问。😊

¹: [Linux Security Modules - Wikipedia](^1^)
²: [Linux Security Modules框架源码分析 - 知乎](^2^)
³: [AppArmor - Wikipedia]
⁴: [Security-Enhanced Linux - Wikipedia]
⁵: [Smack (software) - Wikipedia]
: [TOMOYO Linux - Wikipedia]
: [Yama (Linux security module) - Wikipedia]
: [Linux安全机制之LSM - 知乎](^5^)
: [想实现一个linux内核安全功能模块的技术思路是怎样的？ - 知乎](^4^)

Source: Conversation with Bing, 2023/11/17
(1) Linux Security Modules - Wikipedia. https://en.wikipedia.org/wiki/Linux_Security_Modules.
(2) Linux Security Modules框架源码分析 - 知乎 - 知乎专栏. https://zhuanlan.zhihu.com/p/352103792.
(3) Linux安全模块 - Wikiwand. https://www.wikiwand.com/zh-cn/Linux%E5%AE%89%E5%85%A8%E6%A8%A1%E7%B5%84.
(4) 想实现一个linux内核安全功能模块的技术思路是怎样的？ - 知乎. https://www.zhihu.com/question/21637060.
(5) Linux安全机制之LSM - 知乎 - 知乎专栏. https://zhuanlan.zhihu.com/p/599713986.
(6) Linux Security Modules - Wikipedia. https://en.wikipedia.org/wiki/Linux_Security_Modules.
(7) Linux Security Modules框架源码分析 - 知乎 - 知乎专栏. https://zhuanlan.zhihu.com/p/352103792.
(8) Linux安全机制之LSM - 知乎 - 知乎专栏. https://zhuanlan.zhihu.com/p/599713986.
(9) 想实现一个linux内核安全功能模块的技术思路是怎样的？ - 知乎. https://www.zhihu.com/question/21637060.