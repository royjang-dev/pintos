#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static void validate_user_ptr (const void *uaddr);
static void halt (void);
static void exit(int status);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  thread_exit ();
}

static void 
validate_user_ptr(const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr(uaddr))
    exit (-1);
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result)
       : "m" (*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst)
       : "q" (byte));

  return error_code != -1;
}




static void
halt (void)
{
  shutdown_power_off ();
}

static void
exit (int status)
{
  struct thread *curr = thread_current ();
  curr->status = status;

  printf ("%d: exit(%d)\n", curr->tid, status);

  thread_exit ();
}

static tid_t
exec (const char *cmd_line);
