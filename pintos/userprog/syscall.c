#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include "threads/palloc.h"

struct lock file_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static bool syscall_invalid_memory (struct intr_frame *);
static bool valid_user_buffer (const void *buffer, size_t size);
static bool valid_user_string (const char *str);
static void exit_with_status (int status);
static bool valid_fd (uint64_t fd , struct thread *curr);

static tid_t sys_fork(const char *thread_name);
static int sys_exec(const char *file);
static int sys_wait(tid_t pid);
static bool sys_create(const char *file, unsigned initial_size);
static bool sys_remove(const char *file);
static int sys_open(const char *file);
static int sys_filesize(int fd);
static int sys_read(int fd, void *buffer, unsigned size);
static int sys_write(int fd, const void *buffer, unsigned size);
static bool sys_seek(int fd, unsigned position);
static int sys_tell(int fd);
static bool sys_close(int fd);

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

	lock_init(&file_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	/* 사용자가 넘긴 포인터를 커널이 쓰기 전에 먼저 검사한다 (방법 1).
	 * 잘못된 메모리면 접근하지 않고 프로세스를 exit(-1)로 끝낸다. */
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
			sys_fork((char *)f->R.rdi);
			break;
		
		case SYS_EXEC:
			if (sys_exec((char *)f->R.rdi) == -1) {
				exit_with_status(-1);
			}
			break;

		case SYS_WAIT:
			break;

		case SYS_CREATE:
			f->R.rax = sys_create((char *)f->R.rdi, (unsigned)f->R.rsi);
			break;
		
		case SYS_REMOVE:
			f->R.rax = sys_remove((char *)f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = sys_open((char *)f->R.rdi);
			break;

		case SYS_FILESIZE:
			if ((f->R.rax = sys_filesize((int)f->R.rdi)) == -1) {
				exit_with_status(-1);
			}
			break;
		
		case SYS_READ:
			if ((f->R.rax = sys_read((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx)) == -1) {
				exit_with_status(-1);
			}
			break;

		case SYS_WRITE:
			if ((f->R.rax = sys_write((int)f->R.rdi, (void *)f->R.rsi, (unsigned)f->R.rdx)) == -1) {
				exit_with_status(-1);
			}
			break;

		case SYS_SEEK:
			if (!sys_seek((int)f->R.rdi, (unsigned)f->R.rsi)) {
				exit_with_status(-1);
			}
			break;

		case SYS_TELL:
			if ((f->R.rax = sys_tell((int)f->R.rdi)) == -1) {
				exit_with_status(-1);
			}
			break;

		case SYS_CLOSE:
			if (!sys_close((int)f->R.rdi)) {
				exit_with_status(-1);
			}
			break;

		default:
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

static bool
valid_fd (uint64_t fd , struct thread *curr) {
	return 	fd > 1 && 
			fd < PGSIZE / sizeof(struct file *) && 
			curr->file_descriptor[fd] != NULL;
}

static tid_t
sys_fork (const char *thread_name){
	return (tid_t) 0;
}

static int
sys_exec (const char *file) {
	char *fn_copy = palloc_get_page(0);
	if (fn_copy == NULL) {
		return -1;
	}
	strlcpy(fn_copy, (char *) file, PGSIZE);
	return process_exec(fn_copy);
}

static int
sys_wait (tid_t pid) {
	return 0;
}

static bool
sys_create (const char *file, unsigned initial_size) {
	bool is_create = false;
	lock_acquire(&file_lock);
	is_create = filesys_create(file, initial_size);
	lock_release(&file_lock);
	return is_create;
}

static bool
sys_remove (const char *file) {
	bool is_rm = false;
	lock_acquire(&file_lock);
	is_rm = filesys_remove(file);
	lock_release(&file_lock);
	return is_rm;
}

static int
sys_open (const char *file) {
	int fd = -1;
	struct thread *curr = thread_current();
	lock_acquire(&file_lock);
	struct file *opened_file = filesys_open(file);
	lock_release(&file_lock);

	if (opened_file != NULL) {
		for (int i = 2; i < PGSIZE / sizeof(struct file *); i++) {
			if (curr->file_descriptor[i] == NULL) {
				fd = i;
				curr->file_descriptor[i] = opened_file;						
				break;
			}
		}
		if (fd == -1) {
			lock_acquire(&file_lock);
			file_close(opened_file);
			lock_release(&file_lock);
		}
	}
	return fd;
}

static int
sys_filesize (int fd) {
	struct thread *curr = thread_current();
	if (valid_fd(fd, curr)) {
		return file_length(curr->file_descriptor[fd]);
	}
	else {
		return -1;
	}
}

static int
sys_read (int fd, void *buffer, unsigned size) {
	struct thread *curr = thread_current();
	if ((int) fd == 0) {
		return input_getc();
	}
	else if (valid_fd(fd, curr)) {
		int read_size = -1;
		lock_acquire(&file_lock);
		read_size = file_read(curr->file_descriptor[fd], buffer, size);
		lock_release(&file_lock);
		return read_size;
	}
	else {
		return -1;
	}
}

static int
sys_write (int fd, const void *buffer, unsigned size) {
	struct thread *curr = thread_current();
	if (fd == 1) {
		putbuf (buffer, size);
		return size;
	}
	else if (valid_fd(fd, curr)) {
		int write_size = 0;
		lock_acquire(&file_lock);
		write_size = (int) file_write(curr->file_descriptor[fd], buffer, size);
		lock_release(&file_lock);
		return write_size;
	}
	else {
		return -1;
	}
}

static bool
sys_seek (int fd, unsigned position) {
	struct thread *curr = thread_current();
	if (valid_fd(fd, curr)) {
		file_seek(curr->file_descriptor[fd], position);
		return true;
	} 
	else {
		return false;
	}
}

static int
sys_tell (int fd) {
	struct thread *curr = thread_current();
	if (valid_fd(fd, curr)) {
		return file_tell(curr->file_descriptor[fd]);
	} 
	else {
		return -1;
	}
}

static bool
sys_close (int fd) {
	struct thread *curr = thread_current();
	if (valid_fd(fd, curr)) {
		lock_acquire(&file_lock);
		file_close(curr->file_descriptor[fd]);
		lock_release(&file_lock);
		curr->file_descriptor[fd] = NULL;
		return true;
	}
	else {
		return false;
	}
}
