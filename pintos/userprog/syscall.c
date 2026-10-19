#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static bool valid_user_buffer (const void *buffer, unsigned size);

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
// 시스템 콜 발생했을 때 저장된 사용자 프로그램의 CPU 상태를 가리키는 포인터
void
syscall_handler (struct intr_frame *f) {
	uint64_t syscall_no = f->R.rax; // 시스템 콜 번호
    //uint64_t arg1 = f->R.rdi; // 첫 번째 arg (파일 디스크립터)

	switch (syscall_no) {
        case SYS_EXIT: {
            int status = (int) f->R.rdi;
            thread_current()->exit_status = status;
            thread_exit();
        }
		case SYS_WRITE: {
			int fd = (int) f->R.rdi;
            const void *buffer = (const void *) f->R.rsi;
            unsigned size = (unsigned) f->R.rdx;

            if (!valid_user_buffer (buffer, size)) {
                thread_current ()->exit_status = -1;
                thread_exit ();
            }

            if (fd == 1) {
                putbuf (buffer, size);
                f->R.rax = size;
            } else {
                /* 파일 쓰기 미구현 */
                f->R.rax = -1;
            }
            break;
		}

        default:
            thread_exit();
    }

	// printf ("system call!\n");
	// thread_exit ();
}

/* 버퍼 검사 함수 */
static bool
valid_user_buffer (const void *buffer, unsigned size) {
    uintptr_t start = (uintptr_t) buffer;

    /* 길이가 0이면 읽을 바이트가 없습니다. */
    if (size == 0)
        return true;

    /* 주소 계산 오버플로와 커널 주소 영역 진입을 검사합니다. */
    if (buffer == NULL || size - 1 > UINTPTR_MAX - start)
        return false;

    uintptr_t end = start + size - 1;

    if (!is_user_vaddr ((void *) start) ||
        !is_user_vaddr ((void *) end))
        return false;

    /* 버퍼가 걸치는 모든 페이지가 매핑되어 있는지 확인합니다. */
    for (uintptr_t page = pg_round_down (start);
         page <= end;
         page += PGSIZE) {
        if (pml4_get_page (thread_current ()->pml4, (void *) page) == NULL)
            return false;
    }

    return true;
}
