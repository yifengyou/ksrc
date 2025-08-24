obj-m := cbc4k.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

insmod: all
	insmod cbc4k.ko

rmmod:
	rmmod cbc4k

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

test: clean all
	rmmod cbc4k || :
	dmesg -c &> /dev/null
	insmod cbc4k.ko
	dmesg

