obj-m += cipher_rsa.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# EXTRA_CFLAGS="-g -O0"

default:
	./genkey.sh
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f *.h *.pem *.der *.txt

insmod:
	rmmod cipher_rsa || :
	dmesg -c > /dev/null
	insmod cipher_rsa.ko || :
	dmesg

test: clean default insmod

