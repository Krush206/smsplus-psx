# PSXLinux port of SMS Plus 0.9.3.
#
# Cross-built with the mipsel-linux toolchain, converted to ECOFF with
# the PSXLinux kernel's elf2ecoff, and run by the PSXLinux kernel via
# binfmt_ecoff (see README.md).

CC       = mipsel-linux-gcc
E2E      = /opt/PSXLinux/arch/mipsnommu/boot/elf2ecoff
SOFTFP   = /opt/PSXLinux/psxdev/libsoftfp.a

CFLAGS   = -nostdlib -static -G0 -O2 -Wall -fno-pic -mno-abicalls \
	   -mcpu=r3000 -mips1 -msoft-float -fomit-frame-pointer -DNO_MM \
	   -DPSXLINUX=1 -DLSB_FIRST=1 \
	   -I. -I.. -I../cpu -I/opt/PSXLinux/psxdev
LDFLAGS  = -Wl,-T,psxlinux.ld -e _start

CORE     = ../sms.c ../vdp.c ../render.c ../system.c \
	   ../sn76496.c ../fmopl.c ../ym2413.c

OBJS     = sms.o vdp.o render.o system.o sn76496.o fmopl.o ym2413.o \
	   z80.o portlib.o spu.o main.o

all: smsplus.bflt

%.o: ../%.c ../shared.h ../cpu/z80.h
	$(CC) $(CFLAGS) -c -o $@ $<

z80.o: ../cpu/z80.c ../cpu/z80.h ../cpu/cpuintrf.h ../cpu/osd_cpu.h
	$(CC) $(CFLAGS) -c -o $@ $<

portlib.o: portlib.c port.h
	$(CC) $(CFLAGS) -c -o $@ portlib.c

spu.o: spu.c spu.h hw.h port.h
	$(CC) $(CFLAGS) -c -o $@ spu.c

main.o: main.c hw.h spu.h port.h ../shared.h
	$(CC) $(CFLAGS) -c -o $@ main.c

smsplus.elf: $(OBJS) psxlinux.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(SOFTFP) -lgcc

smsplus.bflt: smsplus.elf
	$(E2E) smsplus.elf smsplus.bflt

clean:
	rm -f *.o *.elf *.bflt
