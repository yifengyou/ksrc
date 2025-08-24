obj-m += fsnotify.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# EXTRA_CFLAGS="-g -O0"

default:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f *.h *.pem *.der *.txt

insmod:
	rmmod fsnotify || :
	dmesg -c > /dev/null
	insmod fsnotify.ko || :
	dmesg

test: clean default insmod

