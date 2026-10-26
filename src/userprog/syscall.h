#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"
extern struct lock filesys_lock;  /* Lock for synchronizing access to the file system. */

void syscall_init (void);

#endif /* userprog/syscall.h */
