/*
 * psxsys.h - Minimal raw syscall interface for PSXLinux userspace.
 *
 * Provides the handful of Linux syscalls needed by the game library
 * without requiring a full C library (the mipsel-linux-gcc cross
 * compiler currently has no target libc installed).
 *
 * MIPS o32 syscall convention:
 *   v0 = syscall number, a0-a3 = args 0-3
 *   syscall via "syscall" instruction
 */

#ifndef _PSXSYS_H
#define _PSXSYS_H

typedef unsigned int	uint;
typedef int		sint;
typedef unsigned int	uint32_t;
typedef unsigned short	uint16_t;
typedef unsigned char	uint8_t;
typedef signed short	int16_t;

#define SYS_exit		1
#define SYS_fork		2
#define SYS_read		3
#define SYS_write		4
#define SYS_open		5
#define SYS_close		6
#define SYS_waitpid		7
#define SYS_creat		8
#define SYS_link		9
#define SYS_unlink		10
#define SYS_execve		11
#define SYS_chdir		12
#define SYS_time		13
#define SYS_gettimeofday	78
#define SYS_mknod		14
#define SYS_chmod		15
#define SYS_lseek		19
#define SYS_getpid		20
#define SYS_stat		106
#define SYS_fstat		108
#define SYS_mount		21
#define SYS_ioctl		54
#define SYS_fcntl		55
#define SYS_setuid		46
#define SYS_setgid		47
#define SYS_getuid		47
#define SYS_getgid		50
#define SYS_poll		188
#define SYS_nanosleep		166

/* ---- inline syscall wrappers (MIPS o32) ---- */

static inline sint sys_call0(sint nr)
{
	register sint __v0 asm("v0") = nr;
	register sint __a0 asm("a0");
	__asm__ volatile("syscall"
			 : "=r"(__v0), "=r"(__a0)
			 : "0"(__v0)
			 : "a1", "a2", "a3", "memory");
	return __v0;
}

static inline sint sys_call1(sint nr, sint a)
{
	register sint __v0 asm("v0") = nr;
	register sint __a0 asm("a0") = a;
	__asm__ volatile("syscall"
			 : "=r"(__v0), "=r"(__a0)
			 : "0"(__v0), "r"(__a0)
			 : "a1", "a2", "a3", "memory");
	return __v0;
}

static inline sint sys_call3(sint nr, sint a, sint b, sint c)
{
	register sint __v0 asm("v0") = nr;
	register sint __a0 asm("a0") = a;
	register sint __a1 asm("a1") = b;
	register sint __a2 asm("a2") = c;
	__asm__ volatile("syscall"
			 : "=r"(__v0), "=r"(__a0), "=r"(__a1), "=r"(__a2)
			 : "0"(__v0), "r"(__a0), "r"(__a1), "r"(__a2)
			 : "a3", "memory");
	return __v0;
}

static inline sint sys_open(const char *path, sint flags)
{
	return sys_call3(SYS_open, (sint)path, flags, 0);
}

static inline sint sys_close(sint fd)
{
	return sys_call1(SYS_close, fd);
}

static inline sint sys_read(sint fd, void *buf, sint count)
{
	return sys_call3(SYS_read, fd, (sint)buf, count);
}

static inline sint sys_write(sint fd, const void *buf, sint count)
{
	return sys_call3(SYS_write, fd, (sint)buf, count);
}

static inline sint sys_ioctl(sint fd, sint cmd, sint arg)
{
	return sys_call3(SYS_ioctl, fd, cmd, arg);
}

static inline sint sys_fstat(sint fd, void *buf)
{
	return sys_call3(SYS_fstat, fd, (sint)buf, 0);
}

static inline void sys_exit(sint code)
{
	sys_call1(SYS_exit, code);
	for (;;) ;  /* never returns */
}

/* ---- tiny printf (writes to console) ---- */

sint psx_printf(const char *fmt, ...);

#endif /* _PSXSYS_H */
