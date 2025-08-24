obj-m += cipher_ecc.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# EXTRA_CFLAGS="-g -O0"

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
