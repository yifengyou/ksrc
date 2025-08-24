# cpuidle

## 参考

* <https://www.kernel.org/doc/html/latest/admin-guide/pm/cpuidle.html>

## 相关代码

```shell

drivers/cpuidle
├── coupled.c
├── cpuidle-arm.c
├── cpuidle-at91.c
├── cpuidle-big_little.c
├── cpuidle.c
├── cpuidle-calxeda.c
├── cpuidle-clps711x.c
├── cpuidle-cps.c
├── cpuidle-exynos.c
├── cpuidle.h
├── cpuidle-haltpoll.c
├── cpuidle-kirkwood.c
├── cpuidle-mvebu-v7.c
├── cpuidle-powernv.c
├── cpuidle-psci.c
├── cpuidle-psci-domain.c
├── cpuidle-psci.h
├── cpuidle-pseries.c
├── cpuidle-qcom-spm.c
├── cpuidle-riscv-sbi.c
├── cpuidle-tegra.c
├── cpuidle-ux500.c
├── cpuidle-zynq.c
├── driver.c
├── dt_idle_genpd.c
├── dt_idle_genpd.h
├── dt_idle_states.c
├── dt_idle_states.h
├── governor.c
├── governors
│      ├── gov.h
│      ├── haltpoll.c
│      ├── ladder.c
│      ├── Makefile
│      ├── menu.c
│      └── teo.c
├── Kconfig
├── Kconfig.arm
├── Kconfig.mips
├── Kconfig.powerpc
├── Kconfig.riscv
├── Makefile
├── poll_state.c
└── sysfs.c

drivers/idle
├── intel_idle.c
├── Kconfig
└── Makefile
```

























---
