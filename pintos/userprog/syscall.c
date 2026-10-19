#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static bool syscall_invalid_memory(struct intr_frame *);

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
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	if (syscall_invalid_memory(f)) {
		thread_exit();
	}

	switch (f->R.rax) {
		case SYS_HALT:
			power_off();
			break;

		case SYS_EXIT:
			thread_current()->process_status = (int) f->R.rdi;
			thread_exit();
			break;

		case SYS_WRITE:
			if (f->R.rdi == 1) {
				putbuf(f->R.rsi, f->R.rdx);
			}
			break;
	
		default:
			break;
	}
}

static bool 
syscall_invalid_memory(struct intr_frame *f) {
	bool is_invalid = false;

	uint64_t addr = 0;
	uint64_t size = 0;

	switch(f->R.rax) {
		case SYS_CREATE:
			size = f->R.rsi - 1;
		case SYS_FORK:
		case SYS_EXEC:
		case SYS_REMOVE:
		case SYS_OPEN:
			addr = f->R.rdi;
			break;
		
		case SYS_READ:
		case SYS_WRITE:
			addr = f->R.rsi;
			size = f->R.rdx;
			break;
		default:
			return is_invalid;
	}

	if (addr >= USER_STACK) {
		is_invalid = true;
	}
	else {
		if ((addr + (size - 1)) >= USER_STACK || 
		!pml4_get_page(thread_current()->pml4, (void *) addr) || 
		!pml4_get_page(thread_current()->pml4, (void *) (addr + (size - 1)))) {
			is_invalid = true;
		}
	}
	
	return is_invalid;
}