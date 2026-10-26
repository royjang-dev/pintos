#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
void validate_user_ptr (const void *uaddr, size_t size);
static void halt (void);
void exit (int status);
int write (int fd, const void *buffer, unsigned size);

struct lock filesys_lock;  /* Lock for synchronizing access to the file system. */

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Validates that a user pointer is within the user address space and mapped in the page directory. */
void
validate_user_ptr (const void *uaddr, size_t size)
{
  if (uaddr == NULL || !is_user_vaddr (uaddr) || !is_user_vaddr ((char *)uaddr + size - 1)) 
    {
      exit (-1); 
    }

  struct thread *cur = thread_current ();
  const void *curr_page = pg_round_down (uaddr); 
  const void *last_page = pg_round_down ((char *)uaddr + size - 1);

  while (curr_page <= last_page) 
    {
      if (pagedir_get_page (cur->pagedir, curr_page) == NULL) 
        {
          exit (-1); 
        }
      curr_page = (char *)curr_page + PGSIZE; 
    }
}

static void
syscall_handler (struct intr_frame *f) 
{
  validate_user_ptr (f->esp, sizeof(uint32_t));

  int syscall_num = *(int *)f->esp;

  switch (syscall_num) 
    {
    case SYS_EXIT:
      validate_user_ptr ((uint32_t *)f->esp + 1, sizeof(uint32_t)); 
      exit (*((int *)f->esp + 1)); 
      break;

    case SYS_WRITE: 
      validate_user_ptr ((uint32_t *)f->esp + 1, sizeof(uint32_t) * 3);
      int fd = *((int *)f->esp + 1);
      const void *buffer = *((const void **)f->esp + 2);
      unsigned size = *((unsigned *)f->esp + 3);

      f->eax = write (fd, buffer, size); 
      break;

    case SYS_HALT:
      halt ();
      break;

    default:
      exit (-1);
    }
}


void exit (int status) 
{
  struct thread *cur = thread_current ();
  cur->exit_status = status;

  printf ("%s: exit(%d)\n", cur->name, status);

  thread_exit ();
}

int write (int fd, const void *buffer, unsigned size) 
{
  validate_user_ptr (buffer, size);

  if (fd == 1) 
    {
      putbuf (buffer, size);
      return size;
    }

  return -1;
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

static tid_t
exec (const char *cmd_line);
