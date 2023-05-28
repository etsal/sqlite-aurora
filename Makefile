SQLITEDIR=$(PWD)/../sqlite
FLAGS=-fPIC -shared -lsls -g
INCLUDEDIR=-I$(SQLITEDIR)/build

install: auroravfs.so
	cp auroravfs.so /usr/local/lib/auroravfs.so

auroravfs.so:
	$(CC) $(INCLUDEDIR) $(FLAGS) src/auroravfs.c -o auroravfs.so

clean:
	rm -f *.so
