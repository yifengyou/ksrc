obj-m := fileblocks.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

insmod: all
	insmod fileblocks.ko

rmmod:
	rmmod fileblocks

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

test: clean all
	rmmod fileblocks || :
	dmesg -c &> /dev/null
	insmod fileblocks.ko
	dmesg

