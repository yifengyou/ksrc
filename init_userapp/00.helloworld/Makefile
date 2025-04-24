
default:
	gcc init.c -static -o init
	echo init | cpio -H newc -o > init.cpio


clean:
	rm -f init init.cpio
