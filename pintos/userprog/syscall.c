#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "userprog/process.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void sys_exit (int status);
static tid_t sys_fork(struct intr_frame *f, const char *thread_name);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static off_t sys_tell (int fd);
static void sys_close (int fd);

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

	case SYS_FORK:
		f->R.rax = sys_fork(f, (uint64_t)f->R.rdi);
		break;

	case SYS_EXEC:
		break;
	
	case SYS_WAIT:
		break;
	
	case SYS_CREATE:
		f->R.rax = sys_create((const char *)f->R.rdi, f->R.rsi);
		break;

	case SYS_REMOVE:
		f->R.rax = sys_remove((const char *)f->R.rdi);
		break;

	case SYS_OPEN:
		f->R.rax = sys_open((const void *)f->R.rdi);
		break;

	case SYS_FILESIZE:
		f->R.rax = sys_filesize(f->R.rdi);
		break;

	case SYS_READ:
		f->R.rax = sys_read(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
		break;

	case SYS_WRITE:
		f->R.rax = sys_write(f->R.rdi, (const void *)f->R.rsi, f->R.rdx);
		break;

	case SYS_SEEK:
		sys_seek(f->R.rdi, f->R.rsi);
		break;

	case SYS_TELL:
		f->R.rax = sys_tell(f->R.rdi);
		break;

	case SYS_CLOSE:
		sys_close(f->R.rdi);
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

static void valid_string(const char *file)
{
	int i = 0;
	while (true)
	{
		check_addr(file + i);
		
		if (file[i] == '\0')
		{
			break;
		}
		
		i++;
	}
}

static tid_t sys_fork(struct intr_frame *f, const char *thread_name)
{
	valid_string(thread_name);
	return process_fork(thread_name, f);
}

void sys_exit(int status)
{
	thread_current()->exit_status = status;
	printf("%s: exit(%d)\n", thread_current()->name, status);
	thread_exit();
}

static bool sys_create(const char *file, unsigned initial_size)
{
	valid_string(file);

	return filesys_create(file, initial_size);
}

static bool sys_remove(const char *file)
{
	if (file == NULL)
	{
		sys_exit(-1);
	}

	valid_string(file);

	check_addr(file);

	return filesys_remove(file);
}

static int sys_open(const char *file)
{
	valid_string(file);

	struct file *file_name = filesys_open(file);
	if (file_name == NULL)
	{
		return -1;
	}

	int fd = fd_add(file_name);

	if (fd != -1)
	{
		return fd;
	}
	else
	{	
		file_close(file_name);
		return -1;
	}
}

static int sys_filesize(int fd)
{
	struct file *file = fd_get(fd);

	if (file == NULL)
	{
		return -1;
	}

	return file_length(file);
}

static int sys_read(int fd, void *buffer, unsigned size)
{
	valid_buffer(buffer, size);

	// 여기 부분 다시 공부하기
	if (fd == 0)
	{
		uint8_t *buf = buffer;

		for (unsigned i = 0; i < size; i++)
		{
			buf[i] = input_getc ();
		}

		return size;
	}

	struct file *file = fd_get(fd);

	if (fd_get(fd) == NULL)
	{
		return -1;
	}

	off_t pos = file_tell(file);
	off_t read_amount = (sys_filesize(fd) - pos < size) ? sys_filesize(fd) - pos : size;

	return file_read(file, buffer, read_amount);
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
	else if (fd < 0 || fd >= PGSIZE / sizeof (struct file *) || fd_get(fd) == NULL)
	{
		return -1;
	}
	else
	{
		return file_write(thread_current()->fd_table[fd], buffer, size);
	}
}

static void sys_seek(int fd, unsigned position)
{
	struct file *file = fd_get(fd);
	
	if (file == NULL || fd_get(fd) == NULL)
	{
		return;
	}

	file_seek(fd_get(fd), position);
}

static off_t sys_tell(int fd)
{
	if (fd_get(fd) == NULL)
	{
		return -1;
	}

	return file_tell(fd_get(fd));
}

static void sys_close(int fd)
{
	fd_close(fd);
}