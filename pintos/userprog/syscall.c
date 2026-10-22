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
#include <stddef.h>
#include "threads/palloc.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

void sys_exit (int status);
static tid_t sys_fork(struct intr_frame *f, const char *thread_name);
static void sys_exec(const char *file);
static int sys_wait(tid_t pid);
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

struct lock lock;

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

	lock_init(&lock);
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
		f->R.rax = sys_fork(f, (const char *)f->R.rdi);
		break;

	case SYS_EXEC:
		sys_exec((const char *)f->R.rdi);
		break;
	
	case SYS_WAIT:
		f->R.rax = sys_wait(f->R.rdi);
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
	
	lock_acquire(&lock);
	file_close(file);
	lock_release(&lock);

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

static void sys_exec(const char *file)
{
	valid_string(file);
	char *name = palloc_get_page(0);
	
	if (name == NULL)
	{
		sys_exit(-1);
	}
	
	strlcpy(name, file, PGSIZE);

	if (process_exec(name) == -1)
	{
		sys_exit(-1);
	}
}

static int sys_wait(tid_t pid)
{
	return process_wait(pid);
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

	lock_acquire(&lock);
	bool succ = filesys_create(file, initial_size);
	lock_release(&lock);

	return succ;
}

static bool sys_remove(const char *file)
{
	if (file == NULL)
	{
		sys_exit(-1);
	}

	valid_string(file);
	check_addr(file);

	lock_acquire(&lock);
	bool succ = filesys_remove(file);
	lock_release(&lock);

	return succ;
}

static int sys_open(const char *file)
{
	valid_string(file);

	lock_acquire(&lock);
	struct file *file_name = filesys_open(file);
	lock_release(&lock);

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
		lock_acquire(&lock);
		file_close(file_name);
		lock_release(&lock);

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

	lock_acquire(&lock);
	off_t file_len = file_length(file);
	lock_release(&lock);

	return file_len;
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

	lock_acquire(&lock);
	off_t pos = file_tell(file);
	lock_release(&lock);
	
	off_t read_amount = (sys_filesize(fd) - pos < size) ? sys_filesize(fd) - pos : size;
	
	lock_acquire(&lock);
	off_t read = file_read(file, buffer, read_amount);
	lock_release(&lock);

	return read;
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
		lock_acquire(&lock);
		off_t write = file_write(thread_current()->fd_table[fd], buffer, size);
		lock_release(&lock);

		return write;
	}
}

static void sys_seek(int fd, unsigned position)
{
	struct file *file = fd_get(fd);
	
	if (file == NULL || fd_get(fd) == NULL)
	{
		return;
	}

	lock_acquire(&lock);
	file_seek(fd_get(fd), position);
	lock_release(&lock);
}

static off_t sys_tell(int fd)
{
	if (fd_get(fd) == NULL)
	{
		return -1;
	}

	lock_acquire(&lock);
	off_t pos = file_tell(fd_get(fd));
	lock_release(&lock);

	return pos;
}

static void sys_close(int fd)
{
	fd_close(fd);
}