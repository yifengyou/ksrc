obj-m := gcm4k.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

insmod: all
	insmod gcm4k.ko

rmmod:
	rmmod gcm4k

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

test: clean all
	rmmod gcm4k || :
	dmesg -c &> /dev/null
	insmod gcm4k.ko
	dmesg

