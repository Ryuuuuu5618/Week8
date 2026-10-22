#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif
#include "threads/synch.h"

/* 한 명령어에서 받을 최대 인자 수 (argv 배열이 커널 스택에 잡히므로 너무 크게 두지 않음) */
#define MAX_ARGS 64

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

static bool setup_arguments (int argc, char *argv[], struct intr_frame *if_);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 스레드 이름은 프로그램 이름(첫 단어)만 사용 (종료 메시지에 쓰임).
	 * file_name은 const라 별도 버퍼에 복사한 뒤 자르고,
	 * fn_copy는 인자 전달을 위해 전체 명령어를 그대로 유지한다. */
	char name_buf[16];
	char *name_save_ptr;
	char *thread_name;
	strlcpy (name_buf, file_name, sizeof name_buf);
	/* 앞에 공백이 있어도 첫 단어를 이름으로 쓰도록 strtok_r의 반환값을 사용 */
	thread_name = strtok_r (name_buf, " ", &name_save_ptr);
	if (thread_name == NULL)
		thread_name = name_buf;

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (thread_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* Clone current thread to new thread.*/

	thread_current()->fork_if = if_;
	
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, thread_current ());
	
	if (tid != TID_ERROR)
	{
		sema_down(&thread_current()->fork_sema);
	}

	return (thread_current()->fork_succ) ? tid : TID_ERROR;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va))
	{
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
	{
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */

	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
	{
		return false;
	}
	
	/* 4. TODO: Duplicate parent's page to the new page and
	*    TODO: check whether parent's page is writable or not (set WRITABLE
	*    TODO: according to the result). */

	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);

		return false;
	}

	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = parent->fork_if;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	for (int fd = 2; fd < PGSIZE / sizeof(struct file *); fd++)
	{
		if (parent->fd_table[fd] == NULL)
		{
			continue;
		}
		
		lock_acquire(&filesys_lock);
		struct file *tmp = file_duplicate(parent->fd_table[fd]);
		lock_release(&filesys_lock);

		if (tmp == NULL)
		{
			goto error;
		}

		current->fd_table[fd] = tmp;
	}

	/* 실행 중인 파일도 복제 (file_duplicate가 쓰기 금지 상태까지 복사한다) */
	if (parent->running_file != NULL)
	{
		lock_acquire(&filesys_lock);
		current->running_file = file_duplicate(parent->running_file);
		lock_release(&filesys_lock);

		if (current->running_file == NULL)
		{
			goto error;
		}
	}

	process_init ();

	if_.R.rax = 0;

	/* Finally, switch to the newly created process. */
	if (succ)
	{
		/* 복제 성공을 부모에게 알리고 깨운다 (do_iret 이후로는 돌아오지 않음) */
		parent->fork_succ = true;

		sema_up(&parent->fork_sema);
		do_iret (&if_);
	}
error:
	/* 복제 실패를 부모에게 알리고 깨운다 */
	parent->fork_succ = false;

	sema_up(&parent->fork_sema);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *command_line = f_name;
	char *argv[MAX_ARGS];
	int argc = 0;
	char *save_ptr;
	bool success;

	/* 공백 기준으로 파싱.
	 * 인자가 MAX_ARGS를 넘으면 잘라서 실행하지 않고 실패로 처리한다. */
	for (char *token = strtok_r (command_line, " ", &save_ptr); token != NULL;
			token = strtok_r (NULL, " ", &save_ptr)) {
		if (argc == MAX_ARGS) {
			palloc_free_page (command_line);
			return -1;
		}
		argv[argc++] = token;
	}

	/* 단어가 하나도 없으면 실패 */
	if (argc == 0) {
		palloc_free_page (command_line);
		return -1;
	}

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* 이전 실행 파일을 닫는다 (쓰기 금지도 풀림) */
	lock_acquire(&filesys_lock);
	file_close(thread_current()->running_file);
	thread_current()->running_file = NULL;
	lock_release(&filesys_lock);

	/* 실행 파일은 첫 번째 토큰임 */
	success = load (argv[0], &_if);

	if (success)
		success = setup_arguments(argc, argv, &_if);

	// 인자를 스택에 복사한 후 원본 해제
	palloc_free_page(command_line);
	
	/* If load failed, quit. */
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

static bool
setup_arguments(int argc, char *argv[], struct intr_frame *if_) {
	if (argc < 1 || argc > MAX_ARGS)
		return false;
		
	uintptr_t rsp = (uintptr_t) if_->rsp;	/* 스택 시작 주소 (USER_STACK) */
	char *arg_addresses[MAX_ARGS];			/* 유저 스택에 복사한 문자열 주소 */

	/* 전체 크기가 스택 한 페이지 안에 들어가는지 먼저 확인한다.
	 * 문자열들 + 정렬 패딩(최대 7) + 포인터 칸(argc + 1) + 가짜 반환 주소 */
	size_t total = 0;
	for (int i = 0; i < argc; i++)
		total += strlen (argv[i]) + 1;
	total += 7 + (argc + 1) * sizeof (char *) + sizeof (void *);
	if (total > PGSIZE)
		return false;

	/* 인자 문자열을 스택에 복사한다. */
	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv[i]) + 1;
		rsp -= len;
		memcpy ((void *) rsp, argv[i], len);
		arg_addresses[i] = (char *) rsp;
	}

	/* 포인터 배열을 8바이트 경계에 맞춘다. */
	rsp &= ~((uintptr_t) 7);

	/* argv[argc] = NULL */
	rsp -= sizeof (char *);
	*(char **) rsp = NULL;

	/* argv[i]의 주소(유저 스택 주소)를 스택에 넣는다. */
	for (int i = argc - 1; i >= 0; i--) {
		rsp -= sizeof (char *);
		*(char **) rsp = arg_addresses[i];
	}

	char **user_argv = (char **) rsp;

	/* _start(argc, argv)의 인자를 레지스터로 전달한다. */
	if_->R.rdi = argc;
	if_->R.rsi = (uintptr_t) user_argv;

	/* 가짜 반환 주소를 두고 최종 스택 포인터를 설정한다. */
	rsp -= sizeof (void *);
	*(void **) rsp = NULL;
	if_->rsp = rsp;

	return true;
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */

	/* 자식 목록에서 child_tid를 찾는다. 없으면 내 자식이 아니거나 이미 wait한 것 */
	struct list_elem *child_list_elem = list_begin(&thread_current()->child_list);
	struct thread *child_thread;

	while (child_list_elem != list_end(&thread_current()->child_list))
	{
		child_thread = list_entry(child_list_elem, struct thread, child_elem);
		
		if (child_thread->tid == child_tid)
		{
			break;
		}
		
		child_list_elem = list_next(child_list_elem);
	}

	if (child_list_elem == list_end(&thread_current()->child_list))
	{
		return -1;
	}
	
	/* 자식이 종료할 때까지 기다린다 */
	sema_down(&child_thread->wait_sema);

	int exit_status = child_thread->process_status;

	/* 같은 자식을 두 번 wait하지 않도록 목록에서 빼고, 자식이 사라져도 된다고 알린다 */
	list_remove(child_list_elem);
	sema_up(&child_thread->free_sema);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	/* 종료 메시지 (process_status는 exit()에서 기록, 커널이 죽인 경우 기본값 -1) */
	if (curr->pml4 != NULL)
		printf("%s: exit(%d)\n", curr->name, curr->process_status);

	/* 열린 파일을 모두 닫고 fd 테이블을 해제한다 */
	if (curr->fd_table != NULL)
	{
		for (int fd = 2; fd < PGSIZE / sizeof(struct file *); fd++)
		{
			struct file *file = curr->fd_table[fd];

			if (file != NULL)
			{
				lock_acquire(&filesys_lock);
				file_close(file);
				lock_release(&filesys_lock);
			}
		}

		palloc_free_page(curr->fd_table);
		curr->fd_table = NULL;
	}

	/* 실행 중이던 파일을 닫는다 (쓰기 금지도 풀림) */
	if (curr->running_file != NULL)
	{
		lock_acquire(&filesys_lock);
		file_close(curr->running_file);
		curr->running_file = NULL;
		lock_release(&filesys_lock);
	}

	/* 유저 프로세스(pml4 보유)가 끝나면 기다리는 부모를 깨우고,
	 * 부모가 종료 상태를 가져갈 때까지 struct thread가 사라지지 않게 기다린다 */
	if (curr->pml4 != NULL)
	{
		sema_up (&curr->wait_sema);
		sema_down(&curr->free_sema);
	}

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	lock_acquire(&filesys_lock);
	file = filesys_open (file_name);
	lock_release(&filesys_lock);

	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* 실행 중인 파일은 수정되지 않도록 쓰기 금지 */
	lock_acquire(&filesys_lock);
	file_deny_write(file);
	lock_release(&filesys_lock);

	/* Read and verify executable header. */
	lock_acquire(&filesys_lock);
	off_t read = file_read (file, &ehdr, sizeof ehdr);
	lock_release(&filesys_lock);
	if (read != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}
	
	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		lock_acquire(&filesys_lock);
		off_t file_len = file_length (file);
		lock_release(&filesys_lock);

		if (file_ofs < 0 || file_ofs > file_len)
			goto done;
		
		lock_acquire(&filesys_lock);
		file_seek (file, file_ofs);
		off_t r = file_read (file, &phdr, sizeof phdr);
		lock_release(&filesys_lock);

		if (r != sizeof phdr)
			goto done;

		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;
	/* 성공하면 파일을 닫지 않고 종료(또는 다음 exec)할 때까지 들고 있는다 */
	t->running_file = file;

done:
	/* We arrive here whether the load is successful or not. */
	if (!success)
	{
		lock_acquire(&filesys_lock);
		file_close (file);
		lock_release(&filesys_lock);
	}

	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */

	lock_acquire(&filesys_lock);
	uint64_t file_len = (uint64_t) file_length(file);
	lock_release(&filesys_lock);

	if (phdr->p_offset > file_len)
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	lock_acquire(&filesys_lock);
	file_seek (file, ofs);
	lock_release(&filesys_lock);
	
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		lock_acquire(&filesys_lock);
		off_t read_amount = file_read (file, kpage, page_read_bytes);
		lock_release(&filesys_lock);

		if (read_amount != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
