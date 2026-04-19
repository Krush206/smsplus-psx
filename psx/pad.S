#include "regdef.h"

.text
.globl InitPAD
InitPAD:
li t0, 0x00B0
li t1, 0x12
jr t0

.globl StartPAD
StartPAD:
li t0, 0x00B0
li t1, 0x13
jr t0

.globl StopPAD
StopPAD:
li t0, 0x00B0
li t1, 0x14
jr t0
