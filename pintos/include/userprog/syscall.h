#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

/* 파일 시스템 코드를 임계 구역으로 보호하는 전역 락 (정의는 syscall.c) */
extern struct lock filesys_lock;

void syscall_init (void);

#endif /* userprog/syscall.h */
