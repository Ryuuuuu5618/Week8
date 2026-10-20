#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <syscall-nr.h>
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

struct lock file_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static bool syscall_invalid_memory (struct intr_frame *);
static bool valid_user_buffer (const void *buffer, size_t size);
static bool valid_user_string (const char *str);
static void exit_with_status (int status);
static bool valid_fd (uint64_t fd , struct thread *curr);

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

	struct thread *curr = thread_current();
	struct file *file;

	switch (f->R.rax) {
		case SYS_HALT:
			power_off ();
			break;

		case SYS_EXIT:
			exit_with_status ((int) f->R.rdi);
			break;

		case SYS_FORK:
			break;
		
		case SYS_EXEC:
			break;

		case SYS_WAIT:
			break;

		case SYS_CREATE:
			lock_acquire(&file_lock);
			f->R.rax = filesys_create((char *)f->R.rdi, f->R.rsi);
			lock_release(&file_lock);
			break;
		
		case SYS_REMOVE:
			lock_acquire(&file_lock);
			f->R.rax = filesys_remove((char *)f->R.rdi);
			lock_release(&file_lock);
			break;

		case SYS_OPEN:
			int fd = -1;
			lock_acquire(&file_lock);
			// f->R.rdi는 파일 이름, file는 파일 주소
			file = filesys_open((char *)f->R.rdi);
			lock_release(&file_lock);

			if (file != NULL) {
				for (int i = 2; i < PGSIZE / sizeof(struct file *); i++) {
					if (curr->file_descriptor[i] == NULL) {
						fd = i;
						curr->file_descriptor[i] = file;						
						break;
					}
				}
				if (fd == -1) file_close(file);
			}

			f->R.rax = fd;
			break;

		case SYS_FILESIZE:
			if (valid_fd(f->R.rdi, curr)) {
				f->R.rax = (uint64_t) file_length(curr->file_descriptor[f->R.rdi]);
			}
			else {
				exit_with_status(-1);
			}
			break;
		
		case SYS_READ:
			if ((int) f->R.rdi == 0) {
				f->R.rax = input_getc();
			}
			else if (valid_fd(f->R.rdi, curr)) {
				int read_size = -1;
				lock_acquire(&file_lock);
				read_size = (int) file_read(curr->file_descriptor[f->R.rdi], (void *)f->R.rsi, f->R.rdx);
				lock_release(&file_lock);
				f->R.rax = read_size;
			}
			else {
				exit_with_status(-1);
			}
			break;

		case SYS_WRITE:
			if ((int) f->R.rdi == 1) {
				putbuf ((const char *) f->R.rsi, (size_t) f->R.rdx);
				f->R.rax = f->R.rdx;
			}
			else if (valid_fd(f->R.rdi, curr)) {
				int write_size = 0;
				lock_acquire(&file_lock);
				write_size = (int) file_write(curr->file_descriptor[f->R.rdi], (void *)f->R.rsi, f->R.rdx);
				lock_release(&file_lock);
				f->R.rax = write_size;
			}
			else {
				exit_with_status(-1);
			}
			break;

		case SYS_SEEK:
			if (valid_fd(f->R.rdi, curr)) {
				file_seek(curr->file_descriptor[f->R.rdi], f->R.rsi);
			} 
			else {
				exit_with_status(-1);
			}
			break;

		case SYS_TELL:
			if (valid_fd(f->R.rdi, curr)) {
				f->R.rax = file_tell(curr->file_descriptor[f->R.rdi]);
			} 
			else {
				exit_with_status(-1);
			}
			break;

		case SYS_CLOSE:
			if (valid_fd(f->R.rdi, curr)) {
				lock_acquire(&file_lock);
				file_close(curr->file_descriptor[f->R.rdi]);
				lock_release(&file_lock);
				curr->file_descriptor[f->R.rdi] = NULL;
			}
			else {
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