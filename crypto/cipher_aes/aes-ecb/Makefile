obj-m := aes.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	make -C $(KDIR) M=$(PWD) modules


clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f *.h *.pem *.der *.txt

insmod:
	rmmod aes || :
	dmesg -c > /dev/null
	insmod aes.ko || :
	dmesg

test: clean default insmod

