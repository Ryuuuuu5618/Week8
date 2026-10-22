#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/fixed-point.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold.
 *
 * First, `struct thread' must not be allowed to grow too big,
 * or there will not be enough room for the kernel stack.
 * Second, kernel stacks must not be allowed to grow too large,
 * or a stack overflow can corrupt the thread's state.
 *
 * The `elem' member can be used in the run queue, a semaphore
 * wait list, or the sleep list.  These uses are mutually exclusive. */
struct thread {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    int priority;                       /* Base priority set by the thread. */
    int eff_priority;                   /* Effective priority, including donations. */
    int nice;
    fixed_t recent_cpu;
    int64_t wakeup_tick;                /* Tick at which to wake up. */
    struct list_elem donator_elem;      /* Element in the lock holder's donations. */
    struct list donations;             /* Threads donating to this thread. */
    struct lock *waiting_lock;          /* Lock this thread is waiting for. */
    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */
    struct list_elem threads_elem;
    int process_status;                 /*프로세스 상태*/

    /* 파일 디스크립터 테이블 (fd = 인덱스, 0/1은 표준 입출력 자리) */
    struct file **fd_table;

    /* fork: 부모의 유저 레지스터, 복제 완료 대기, 복제 결과 */
    struct intr_frame *fork_if;
    struct semaphore fork_sema;
    bool fork_succ;

    /* wait: 부모-자식 연결, 종료 대기, 해제 대기 */
    struct list child_list;
    struct list_elem child_elem;
    struct thread *parent;
    struct semaphore wait_sema;
    struct semaphore free_sema;

    /* 실행 중인 파일 (종료할 때까지 열어 두고 쓰기 금지) */
    struct file *running_file;

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
    /* Table for whole virtual memory owned by thread. */
    struct supplemental_page_table spt;
#endif

    /* Owned by thread.c. */
    struct intr_frame tf;               /* Information for switching. */
    unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);
void thread_sleep(int64_t wakeup_tick);
void thread_wakeup(int64_t ticks);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

bool priority_first(const struct list_elem *, const struct list_elem *, void *aux);
void thread_preempt(void);
void thread_refresh_priority(struct thread *);
void thread_donate_priority(void);

#endif /* threads/thread.h */
