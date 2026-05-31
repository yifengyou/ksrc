# Introduction

RAS 特性全景图**

| 类别 | 机制 | 主要目的 |
|------|------|--------|
| **错误检测** | EDAC, APEI, ARM64 RAS, MCE | 发现硬件错误 |
| **故障响应** | panic(), memory_failure(), CPU offline | 隔离/终止故障 |
| **诊断取证** | kdump, pstore, ftrace, KASAN | 保留现场信息 |


