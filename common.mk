CC = mipsel-unknown-ecoff-gcc
LD = mipsel-unknown-ecoff-ld
CFLAGS = -c -I$(PWD) -I$(PWD)/cpu -DPSX=1
OPT = -O2
