include ../common.mk

SRC =	./psx.c
OBJ =	$(SRC:.c=.o)

all: $(OBJ) pad.o

%.o: %.c
	$(CC) $(CFLAGS) $(OPT) $<

pad.o: pad.S
	$(CC) $(CFLAGS) $(OPT) $<

clean:
	rm -f ./*.o
