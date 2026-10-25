#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
<<<<<<< HEAD
#include "devices/shutdown.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static void validate_user_ptr (const void *uaddr);
static void halt (void);
static void exit(int status);
=======
#include "userprog/pagedir.h" 

static void syscall_handler (struct intr_frame *);
void exit (int status);
int write (int fd, const void *buffer, unsigned size);
void validate_user_ptr (const void *uaddr, size_t size);
>>>>>>> cabfbcc (argument passing)

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

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

<<<<<<< HEAD
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
=======

int write (int fd, const void *buffer, unsigned size) 
{
  validate_user_ptr (buffer, size); 

  if (fd == 1) 
  {
    putbuf (buffer, size);
    return size;
  }
    
  // 추후 파일 시스템(Project 2 후반부)을 구현하면 fd가 1이 아닐 때 
  // 실제 파일에 쓰는 로직(file_write)이 여기에 추가
  return -1;
}

>>>>>>> cabfbcc (argument passing)
