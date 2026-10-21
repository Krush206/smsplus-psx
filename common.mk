CC = mipsel-ecoff-gcc
LD = mipsel-ecoff-ld
CFLAGS = -Wall -c -I$(PWD) -I$(PWD)/cpu -DPSX=1 -msoft-float -mno-mips-tfile
OPT = -O2
