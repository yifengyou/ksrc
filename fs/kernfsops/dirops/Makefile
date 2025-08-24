obj-m += dirops.o

KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

insmod:
	rmmod dirops || :
	dmesg -c > /dev/null
	insmod dirops.ko || :
	dmesg

test: clean default insmod

