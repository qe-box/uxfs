


CC	= gcc
CFLAGS	= -O2 -Wall -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
#CFLAGS	= -Wall -ggdb -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
LDFLAGS	= -lfuse -pthread


SRC	= uxfs.o


uxfs:	$(SRC)
	$(CC) -o $@ $(SRC) $(LDFLAGS)
	ctags *.[ch]

ctags:
	ctags *.[ch]

clean:
	rm uxfs *.o

.PHONY:	ctags clean

