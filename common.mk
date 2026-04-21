CC = mipsel-unknown-ecoff-gcc
LD = mipsel-unknown-ecoff-ld
CFLAGS = -Wall -c -I$(PWD) -I$(PWD)/cpu -DPSX=1 -msoft-float -mno-mips-tfile
OPT = -O2
