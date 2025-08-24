obj-m += cipher_sm2.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# EXTRA_CFLAGS="-g -O0"

default:
	./genkey.sh
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f *.h *.pem *.der *.txt

insmod:
	rmmod cipher_sm2 || :
	dmesg -c > /dev/null
	insmod cipher_sm2.ko || :
	dmesg

test: clean default insmod

