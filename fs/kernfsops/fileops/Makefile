obj-m += kernfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

insmod:
	rmmod kernfs || :
	dmesg -c > /dev/null
	insmod kernfs.ko || :
	dmesg

test: clean default insmod

