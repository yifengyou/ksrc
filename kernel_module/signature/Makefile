obj-m += skip_check.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# 禁用BTF生成
ccflags-y := -DDEBUG -D__KERNEL__ -DMODULE
KBUILD_CFLAGS_MODULE += -fno-inline -fno-stack-protector -fno-pic
CONFIG_DEBUG_INFO_BTF := n

# EXTRA_CFLAGS="-g -O0"

default:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

insmod:
	rmmod skip_check || :
	dmesg -c > /dev/null
	insmod skip_check.ko || :
	dmesg

test: clean default insmod

