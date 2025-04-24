ENTRY := slab
obj-m := $(ENTRY).o
KERNEL_VER = $(shell uname -r)
default: notfound_build
  
force_build: slab.c
	make -C /lib/modules/$(KERNEL_VER)/build M=$(PWD) modules
	ls -alh *.ko

notfound_build: slab.c
	[ -f *.ko ] || make -C /lib/modules/$(KERNEL_VER)/build M=$(PWD) modules

build: force_build

clean:
	make -C /lib/modules/$(KERNEL_VER)/build M=$(PWD) clean

insmod: info notfound_build
	dmesg --clear
	insmod slab.ko || true
	dmesg

rmmod:
	rmmod slab && dmesg

lsmod:
	lsmod |grep slab

status: lsmod

info: notfound_build slab.ko
	modinfo slab.ko
	md5sum slab.ko

modinfo: info

help:
	@echo " build    - build module(default target)"
	@echo " clean    - clean build dir"
	@echo " insmod   - insmod slab ko module"
	@echo " rmmod    - rmmod slab ko module"
	@echo " lsmod    - find slab ko module whether already insmod"
	@echo " status   - same as lsmod"
	@echo " info     - display slab ko info"
	@echo " modinfo  - same as info"
	@echo " help     - display help info"

