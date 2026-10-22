#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

extern struct lock lock;

void syscall_init (void);
void sys_exit(int status);

#endif /* userprog/syscall.h */
