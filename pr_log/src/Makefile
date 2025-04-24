ENTRY := pr_log
obj-m := $(ENTRY).o
KERNEL_VER = $(shell uname -r)
default: notfound_build
  
force_build: pr_log.c
	make -C /lib/modules/$(KERNEL_VER)/build M=$(PWD) modules
	ls -alh *.ko

notfound_build: pr_log.c
	[ -f *.ko ] || make -C /lib/modules/$(KERNEL_VER)/build M=$(PWD) modules

build: force_build

clean:
	make -C /lib/modules/$(KERNEL_VER)/build M=$(PWD) clean

insmod: info notfound_build
	dmesg --clear
	insmod pr_log.ko || true
	dmesg

rmmod:
	rmmod pr_log && dmesg

lsmod:
	lsmod |grep pr_log

status: lsmod

info: notfound_build pr_log.ko
	modinfo pr_log.ko
	md5sum pr_log.ko

modinfo: info

help:
	@echo " build    - build module(default target)"
	@echo " clean    - clean build dir"
	@echo " insmod   - insmod pr_log ko module"
	@echo " rmmod    - rmmod pr_log ko module"
	@echo " lsmod    - find pr_log ko module whether already insmod"
	@echo " status   - same as lsmod"
	@echo " info     - display pr_log ko info"
	@echo " modinfo  - same as info"
	@echo " help     - display help info"

