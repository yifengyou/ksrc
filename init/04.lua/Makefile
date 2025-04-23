.PHONY = lua default clean

default: shell lua
	chmod +x init lua
	echo init > filelist
	echo lua >> filelist
	cat filelist | cpio -H newc -o > init.cpio
	cpio -tv < init.cpio

shell:
	gcc -static shell.c -o init

lua:
	wget -c https://www.lua.org/ftp/lua-5.4.7.tar.gz
	tar -xvf lua-5.4.7.tar.gz
	sed -i 's/^MYLDFLAG.*/MYLDFLAGS=-static/' lua-5.4.7/src/Makefile
	cd lua-5.4.7/src/ && make -j`nproc`
	cp -a lua-5.4.7/src/lua .
	ls -alh lua
	ldd lua || :

clean:
	rm -rf *.o lua init shell *.cpio filelist lua-5.4.7

