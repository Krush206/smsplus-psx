include ./common.mk

SRC =	./render.c \
	./ym2413.c \
	./sn76496.c \
	./vdp.c \
	./system.c \
	./fmopl.c \
	./sms.c
OBJ =	$(SRC:.c=.o)

all: $(OBJ) cpu psx
	$(LD) -r ./*.o \
		 ./psx/*.o \
		 ./cpu/*.o \
		 /usr/local/mipsel-unknown-ecoff/lib/libps.a \
		 /usr/local/lib/gcc-lib/mipsel-unknown-ecoff/egcs-2.91.66/libgcc.a
	$(LD) -o smsplus -Ttext 0x80100000 --entry main a.out

%.o: %.c
	$(CC) $(CFLAGS) $(OPT) $<

cpu:
	$(MAKE) -C ./cpu

psx:
	$(MAKE) -C ./psx

clean:
	$(MAKE) -C ./cpu clean
	$(MAKE) -C ./psx clean
	rm -f ./*.o
	rm -f ./smsplus
