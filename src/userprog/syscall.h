#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/thread.h"
#include "threads/synch.h"
extern struct lock filesys_lock;  /* Lock for synchronizing access to the file system. */

void syscall_init (void);
struct pipe;

struct pipe *pipe_find_pair (struct thread *t);
void pipe_add_reader (struct pipe *p);
void pipe_drop_reader (struct pipe *p);
bool fd_install_pipe_reader (struct thread *t, int fd, struct pipe *p);
void fd_table_close_all (struct thread *t);
#endif /* userprog/syscall.h */
