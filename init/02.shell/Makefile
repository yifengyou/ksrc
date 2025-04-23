
default:
	gcc -static shell.c -o init
	echo init | cpio -H newc -o > init.cpio

clean:
	rm -f *.o init shell *.cpio


