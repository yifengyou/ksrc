obj-m := find_symby_kprobe.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

insmod: all
	insmod find_symby_kprobe.ko

rmmod:
	rmmod find_symby_kprobe

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

test: clean all
	rmmod find_symby_kprobe || :
	dmesg -c &> /dev/null
	insmod find_symby_kprobe.ko
	dmesg

