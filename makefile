include ./common.mk

SRC =	./render.c \
	./ym2413.c \
	./sn76496.c \
	./vdp.c \
	./system.c \
	./fmopl.c \
	./sms.c
OBJ =	$(SRC:.c=.o)

all: $(OBJ)
	$(MAKE) -C ./cpu
	$(MAKE) -C ./psx
	$(LD) -o smsplus \
		 -T/usr/local/mipsel-ecoff/lib/ldscripts/mipspsx.x \
		 ./*.o \
		 ./psx/*.o \
		 ./cpu/*.o \
		 libps_patched.a \
		 /usr/local/lib/gcc-lib/mipsel-ecoff/egcs-2.91.66/libgcc.a

%.o: %.c
	$(CC) $(CFLAGS) $(OPT) $<

clean:
	$(MAKE) -C ./cpu clean
	$(MAKE) -C ./psx clean
	rm -f ./*.o
	rm -f ./smsplus
