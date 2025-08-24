obj-m := aes4k.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

insmod: all
	insmod aes4k.ko

rmmod:
	rmmod aes4k

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

test: clean all
	rmmod aes4k || :
	dmesg -c &> /dev/null
	insmod aes4k.ko
	dmesg

