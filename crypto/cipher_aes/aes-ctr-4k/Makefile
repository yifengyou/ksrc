obj-m := ctr4k.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

insmod: all
	insmod ctr4k.ko

rmmod:
	rmmod ctr4k

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

test: clean all
	rmmod ctr4k || :
	dmesg -c &> /dev/null
	insmod ctr4k.ko
	dmesg

