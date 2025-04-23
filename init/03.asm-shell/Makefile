
default:
	gcc -c shell.c -o shell.o
	as sys.S -o sys.o
	ld sys.o shell.o -o shell --entry main -z noexecstack
	ls -alh shell
	mv shell init
	echo init | cpio -H newc -o > init.cpio

clean:
	rm -f *.o init shell *.cpio


