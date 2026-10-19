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
#include "threads/palloc.h"
#include "userprog/process.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static bool syscall_invalid_memory (struct intr_frame *);
static bool valid_user_buffer (const void *buffer, size_t size);
static bool valid_user_string (const char *str);
static void exit_with_status (int status);

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

		case SYS_WRITE:
			/* TODO: 파일 쓰기(fd >= 2)는 파일 시스템 콜 작업에서 구현 */
			if ((int) f->R.rdi == 1) {
				putbuf ((const char *) f->R.rsi, (size_t) f->R.rdx);
				f->R.rax = f->R.rdx;
			}
			break;
		case SYS_EXEC: { // 현재 프로세스를 새 프로그램으로 바꿈
			// 사용자 메모리에 커맨드 라인의 문자열 주소 넣어줌
			const char *user_cmd = (const char *) f->R.rdi;
			// 커맨드라인 복사용 커널 메모리 페이지 할당. 파싱하면서 데이터가 훼손되므로 원본 대신 복사본 넘기려고 하는 거임
            char *kernel_cmd = palloc_get_page (0);
			
            if (kernel_cmd == NULL) // 실패하면 복사 못함. -1
                exit_with_status (-1);

            /* 사용자 명령줄을 커널 페이지에 복사합니다. */
            size_t i;
            for (i = 0; i < PGSIZE; i++) { // 문자열 끝 or 페이지 크기만큼 복사
				// 의문: syscall_invalid_memory()가 사용자 문자열의 주소와 매핑을 검사... 한다는데 어떻게 하는거임?
            	kernel_cmd[i] = user_cmd[i];
                if (kernel_cmd[i] == '\0')
                    break;
            }

            /* 한 페이지 안에 문자열 끝이 없으면 실패 처리합니다. */
            if (i == PGSIZE) {
                palloc_free_page (kernel_cmd);
                exit_with_status (-1);
            }

            /* 성공하면 현재 프로세스가 새 프로그램으로 바뀝니다. */
            if (process_exec (kernel_cmd) < 0)
                exit_with_status (-1);

            break;
		}
			
			

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
