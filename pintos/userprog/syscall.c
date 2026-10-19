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

static int sys_write(int fd, const void *buffer, unsigned size);
void sys_exit(int status);

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
	// TODO: Your implementation goes here.

	switch (f->R.rax)
	{
	case SYS_HALT:
		power_off();
		break;
	
	case SYS_EXIT:
		sys_exit(f->R.rdi);
		break;
	
	case SYS_CREATE:
		break;

	case SYS_OPEN:
		break;

	case SYS_WRITE:
		f->R.rax = sys_write(f->R.rdi, (const void *)f->R.rsi, f->R.rdx);
		break;
	
	default:
		sys_exit(-1);
		break;
	}

	// printf ("system call!\n");
	// thread_exit ();
}

static struct file *fd_get(int fd)
{
	if (fd < 0 || fd >= PGSIZE / sizeof (struct file *))
	{
		return NULL;
	}

	return thread_current()->fd_table[fd];
}

static int fd_add(struct file *file)
{
	for (int fd = 2; fd < PGSIZE / sizeof (struct file *); fd++)
	{
		if (thread_current()->fd_table[fd] == NULL)
		{
			thread_current()->fd_table[fd] = file;
			return fd;
		}
	}

	return -1;
}

static void fd_close(int fd)
{
	struct file *file = fd_get(fd);

	if (file == NULL)
	{
		return;
	}

	file_close(file);
	thread_current()->fd_table[fd] = NULL;
}

static int sys_open(const char *file)
{
	struct file *file_name = filesys_open(file);

	return (fd_get(file_name) != NULL) ? fd_get(file_name) : -1;
}


static void check_addr(const void *buffer)
{
	// 버퍼 주소 검증
	if (buffer == NULL || (!is_user_vaddr(buffer)) || !pml4_get_page(thread_current()->pml4, buffer))
	{
		sys_exit(-1);
	}
}

static void valid_buffer(const void *buffer, unsigned size)
{
	if (size == 0)
	{
		check_addr(buffer);
		return;
	}

	for (void *addr = pg_round_down(buffer); addr < buffer + size; addr += PGSIZE)
	{
		check_addr(addr);
	}
}

static int sys_write(int fd, const void *buffer, unsigned size)
{
	// 버퍼 주소 검증
	valid_buffer(buffer, size);

	// 화면 출력 -> fd가 1
	if (fd == 1)
	{
		putbuf(buffer, size);
		return size;
	}
	else if (fd < 0 || fd >= PGSIZE / sizeof (struct file *))
	{
		return -1;
	}
	else
	{
		file_write(thread_current()->fd_table[fd], buffer, size);
	}
}

void sys_exit(int status)
{
	thread_current()->exit_status = status;
	printf("%s: exit(%d)\n", thread_current()->name, status);
	thread_exit();
}

