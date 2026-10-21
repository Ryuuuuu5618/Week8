#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "threads/flags.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static bool syscall_invalid_memory (struct intr_frame *);
static bool valid_user_buffer (const void *buffer, size_t size);
static bool valid_user_string (const char *str);
static void exit_with_status (int status);

static tid_t sys_fork (struct intr_frame *f, const char *thread_name);
static void sys_exec (const char *file);
static int sys_wait (tid_t pid);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static off_t sys_tell (int fd);
static void sys_close (int fd);

/* fd 테이블 크기 (한 페이지에 들어가는 struct file * 개수 = 512) */
#define FD_MAX ((int) (PGSIZE / sizeof (struct file *)))

/* 파일 시스템 코드를 임계 구역으로 보호하는 전역 락 */
struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init (&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	/* 사용자가 넘긴 포인터를 커널이 쓰기 전에 먼저 검사한다 (방법 1).
	 * 잘못된 메모리면 접근하지 않고 프로세스를 exit(-1)로 끝낸다.
	 * 락을 잡기 전에 검사가 끝나므로, 락을 쥔 채 종료되는 일이 없다. */
	if (syscall_invalid_memory (f)) {
		exit_with_status (-1);
	}

	switch (f->R.rax) {
		case SYS_HALT:
			power_off ();
			break;

		case SYS_EXIT:
			exit_with_status ((int) f->R.rdi);
			break;

		case SYS_FORK:
			f->R.rax = sys_fork (f, (const char *) f->R.rdi);
			break;

		case SYS_EXEC:
			sys_exec ((const char *) f->R.rdi);
			break;

		case SYS_WAIT:
			f->R.rax = sys_wait ((tid_t) f->R.rdi);
			break;

		case SYS_CREATE:
			f->R.rax = sys_create ((const char *) f->R.rdi, (unsigned) f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = sys_remove ((const char *) f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = sys_open ((const char *) f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = sys_filesize ((int) f->R.rdi);
			break;

		case SYS_READ:
			f->R.rax = sys_read ((int) f->R.rdi, (void *) f->R.rsi, (unsigned) f->R.rdx);
			break;

		case SYS_WRITE:
			f->R.rax = sys_write ((int) f->R.rdi, (const void *) f->R.rsi, (unsigned) f->R.rdx);
			break;

		case SYS_SEEK:
			sys_seek ((int) f->R.rdi, (unsigned) f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = sys_tell ((int) f->R.rdi);
			break;

		case SYS_CLOSE:
			sys_close ((int) f->R.rdi);
			break;

		default:
			exit_with_status (-1);
			break;
	}
}

/* 종료 상태를 기록하고 프로세스를 끝낸다.
 * 종료 메시지는 process_exit()에서 이 값을 읽어 출력한다. */
static void
exit_with_status (int status) {
	thread_current ()->process_status = status;
	thread_exit ();
}

/* 시스템 콜이 받은 사용자 메모리가 잘못되었으면 true.
 * 시스템 콜마다 포인터가 들어오는 레지스터와 크기가 다르다 (lib/user/syscall.c). */
static bool
syscall_invalid_memory (struct intr_frame *f) {
	switch (f->R.rax) {
		/* 파일 이름, 명령줄처럼 문자열을 받는 시스템 콜 (첫 번째 인자 rdi).
		 * create의 두 번째 인자는 만들 파일의 크기라 메모리 검사와 관계없다. */
		case SYS_CREATE:
		case SYS_REMOVE:
		case SYS_OPEN:
		case SYS_EXEC:
		case SYS_FORK:
			return !valid_user_string ((const char *) f->R.rdi);

		/* 버퍼와 크기를 받는 시스템 콜 (rsi = 버퍼, rdx = 크기).
		 * TODO: read는 커널이 버퍼에 쓰므로 쓰기 가능한 페이지인지도 확인해야 한다. */
		case SYS_READ:
		case SYS_WRITE:
			return !valid_user_buffer ((const void *) f->R.rsi, (size_t) f->R.rdx);

		default:
			return false;
	}
}

/* BUFFER부터 SIZE바이트가 모두 접근 가능한 사용자 메모리인지 확인한다. */
static bool
valid_user_buffer (const void *buffer, size_t size) {
	uintptr_t start = (uintptr_t) buffer;
	uintptr_t end;

	/* 읽고 쓸 바이트가 없으면 메모리에 접근하지 않는다 (read-zero, write-zero). */
	if (size == 0) {
		return true;
	}

	/* NULL, 주소 + 크기 계산이 넘치는 경우 */
	if (buffer == NULL || size - 1 > UINTPTR_MAX - start) {
		return false;
	}
	end = start + size - 1;

	/* 시작과 끝이 모두 사용자 영역(KERN_BASE 미만)이어야 한다. */
	if (!is_user_vaddr ((void *) start) || !is_user_vaddr ((void *) end)) {
		return false;
	}

	/* 버퍼가 걸친 모든 페이지가 매핑되어 있어야 한다 (중간 페이지 포함). */
	for (uintptr_t page = (uintptr_t) pg_round_down (buffer); page <= end;
			page += PGSIZE) {
		if (pml4_get_page (thread_current ()->pml4, (void *) page) == NULL) {
			return false;
		}
	}
	return true;
}

/* STR부터 '\0'까지의 문자열 전체가 접근 가능한 사용자 메모리인지 확인한다.
 * 문자열은 길이를 미리 모르므로, 한 바이트씩 보면서
 * 처음과 페이지가 바뀔 때마다 사용자 영역인지, 매핑되어 있는지 검사한다. */
static bool
valid_user_string (const char *str) {
	const char *p = str;

	if (str == NULL) {
		return false;
	}

	for (;;) {
		if (p == str || pg_ofs (p) == 0) {
			if (!is_user_vaddr (p)
					|| pml4_get_page (thread_current ()->pml4, p) == NULL) {
				return false;
			}
		}
		if (*p == '\0') {
			return true;
		}
		p++;
	}
}

/* ---------- fd 테이블 도우미 ---------- */

/* fd에 해당하는 열린 파일. 범위 밖이거나 비어 있으면 NULL. */
static struct file *
fd_get (int fd) {
	if (fd < 0 || fd >= FD_MAX) {
		return NULL;
	}
	return thread_current ()->fd_table[fd];
}

/* 빈 칸(2번부터)에 파일을 넣고 fd를 반환. 자리가 없으면 -1. */
static int
fd_add (struct file *file) {
	for (int fd = 2; fd < FD_MAX; fd++) {
		if (thread_current ()->fd_table[fd] == NULL) {
			thread_current ()->fd_table[fd] = file;
			return fd;
		}
	}
	return -1;
}

/* fd의 파일을 닫고 칸을 비운다. */
static void
fd_close (int fd) {
	struct file *file = fd_get (fd);

	if (file == NULL) {
		return;
	}

	lock_acquire (&filesys_lock);
	file_close (file);
	lock_release (&filesys_lock);

	thread_current ()->fd_table[fd] = NULL;
}

/* ---------- 프로세스 시스템 콜 ---------- */

static tid_t
sys_fork (struct intr_frame *f, const char *thread_name) {
	return process_fork (thread_name, f);
}

/* 성공하면 돌아오지 않는다. 실패하면 exit(-1). */
static void
sys_exec (const char *file) {
	/* process_exec이 유저 메모리를 지우고, 마지막에 palloc_free_page로 해제하므로
	 * 명령줄을 커널 페이지에 복사해서 넘긴다. */
	char *name = palloc_get_page (0);

	if (name == NULL) {
		exit_with_status (-1);
	}

	strlcpy (name, file, PGSIZE);

	if (process_exec (name) == -1) {
		exit_with_status (-1);
	}
}

static int
sys_wait (tid_t pid) {
	return process_wait (pid);
}

/* ---------- 파일 시스템 콜 ---------- */
/* 포인터 검사는 syscall_handler 앞단에서 끝났다. 파일 시스템 함수 호출만 락으로 감싼다. */

static bool
sys_create (const char *file, unsigned initial_size) {
	lock_acquire (&filesys_lock);
	bool succ = filesys_create (file, initial_size);
	lock_release (&filesys_lock);

	return succ;
}

static bool
sys_remove (const char *file) {
	lock_acquire (&filesys_lock);
	bool succ = filesys_remove (file);
	lock_release (&filesys_lock);

	return succ;
}

static int
sys_open (const char *file) {
	lock_acquire (&filesys_lock);
	struct file *opened = filesys_open (file);
	lock_release (&filesys_lock);

	if (opened == NULL) {
		return -1;
	}

	int fd = fd_add (opened);

	if (fd == -1) {
		/* fd 테이블에 자리가 없으면 방금 연 파일을 닫는다 */
		lock_acquire (&filesys_lock);
		file_close (opened);
		lock_release (&filesys_lock);
	}

	return fd;
}

static int
sys_filesize (int fd) {
	struct file *file = fd_get (fd);

	if (file == NULL) {
		return -1;
	}

	lock_acquire (&filesys_lock);
	off_t file_len = file_length (file);
	lock_release (&filesys_lock);

	return file_len;
}

static int
sys_read (int fd, void *buffer, unsigned size) {
	/* 키보드: input_getc로 한 글자씩 버퍼에 채운다 */
	if (fd == 0) {
		uint8_t *buf = buffer;

		for (unsigned i = 0; i < size; i++) {
			buf[i] = input_getc ();
		}

		return size;
	}

	struct file *file = fd_get (fd);

	if (file == NULL) {
		return -1;
	}

	/* file_read가 버퍼를 채우고, pos를 옮기고, 실제로 읽은 바이트 수를 돌려준다 */
	lock_acquire (&filesys_lock);
	off_t read = file_read (file, buffer, size);
	lock_release (&filesys_lock);

	return read;
}

static int
sys_write (int fd, const void *buffer, unsigned size) {
	/* 화면 출력 */
	if (fd == 1) {
		putbuf (buffer, size);
		return size;
	}

	struct file *file = fd_get (fd);

	if (file == NULL) {
		return -1;
	}

	lock_acquire (&filesys_lock);
	off_t written = file_write (file, buffer, size);
	lock_release (&filesys_lock);

	return written;
}

static void
sys_seek (int fd, unsigned position) {
	struct file *file = fd_get (fd);

	if (file == NULL) {
		return;
	}

	lock_acquire (&filesys_lock);
	file_seek (file, position);
	lock_release (&filesys_lock);
}

static off_t
sys_tell (int fd) {
	struct file *file = fd_get (fd);

	if (file == NULL) {
		return -1;
	}

	lock_acquire (&filesys_lock);
	off_t pos = file_tell (file);
	lock_release (&filesys_lock);

	return pos;
}

static void
sys_close (int fd) {
	fd_close (fd);
}
