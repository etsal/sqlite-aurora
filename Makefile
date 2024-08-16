SQLITEDIR=$(PWD)/../sqlite
FLAGS=-fPIC -shared -g
INCLUDEDIR=-I$(SQLITEDIR)/build

default: auroravfs.so auroravfs-objsnap.so

install: auroravfs.so auroravfs-objsnap.so
	cp auroravfs.so /usr/local/lib/auroravfs.so
	cp auroravfs-objsnap.so /usr/local/lib/auroravfs-objsnap.so

auroravfs.so: src/auroravfs.c
	$(CC) $(INCLUDEDIR) $(FLAGS) -lsls src/auroravfs.c -o auroravfs.so

auroravfs-objsnap.so: src/auroravfs.c
	$(CC) $(INCLUDEDIR) $(FLAGS) -DUSE_MSNP_OBJSNP -lmsnp src/auroravfs.c -o auroravfs-objsnap.so

clean:
	rm -f *.so
