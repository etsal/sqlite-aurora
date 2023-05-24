SQLITEDIR=$(PWD)/../sqlite
FLAGS=-fPIC -shared
INCLUDEDIR=-I$(SQLITEDIR)/build
auroravfs.so:
	$(CC) $(INCLUDEDIR) $(FLAGS) src/auroravfs.c -o auroravfs.so

clean:
	rm -f *.so
