include ../common.mk

SRC =	./z80.c
OBJ =	$(SRC:.c=.o)

all: $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(OPT) $<

clean:
	rm -f ./*.o
