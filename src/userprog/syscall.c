#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "lib/limits.h"

#define MAX_FD 128
#define ARG(n) read_user_word ((uint32_t *) f->esp + (n))

static void syscall_handler (struct intr_frame *);
// void validate_user_ptr (const void *uaddr, size_t size);
static int sys_write (int fd, const void *buffer, unsigned size);
static int sys_read (int fd, void *buffer, unsigned size);
static bool sys_create (const char *file, unsigned initial_size);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static int sys_exec (const char *cmd_line);
static void sys_exit (int status);
static void sys_close (int fd);
static void sys_halt (void);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);

struct lock filesys_lock;  /* Lock for synchronizing access to the file system. */

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// /* Validates that a user pointer is within the user address space and mapped in the page directory. */
// void
// validate_user_ptr (const void *uaddr, size_t size)
// {
//   if (uaddr == NULL || !is_user_vaddr (uaddr) || !is_user_vaddr ((char *)uaddr + size - 1)) 
//     {
//       exit (-1); 
//     }

//   struct thread *cur = thread_current ();
//   const void *curr_page = pg_round_down (uaddr);
//   const void *last_page = pg_round_down ((char *)uaddr + size - 1);

//   while (curr_page <= last_page) 
//     {
//       if (pagedir_get_page (cur->pagedir, curr_page) == NULL) 
//         {
//           exit (-1); 
//         }
//       curr_page = (char *)curr_page + PGSIZE; 
//     }
// }

/* Reads a byte at user virtual address UADDR. UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static uint8_t
read_user_byte (const void *uaddr)
{
  if (!is_user_vaddr (uaddr))
    sys_exit (-1);
  int b = get_user (uaddr);
  if (b == -1)
    sys_exit (-1);
  return b;
}

static uint32_t
read_user_word (const void *uaddr) 
{
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= (uint32_t) read_user_byte ((const uint8_t *) uaddr + i) << (8 * i);
  return v;
}

static void
check_user_string (const char *s)
{
  for (const char *p = s; read_user_byte (p) != '\0'; p++)
    continue;
}

/* Checks that a user buffer is valid for access.
   BUF is the starting address, SIZE is the number of bytes,
   and WRITABLE indicates if the buffer will be written to.
   Exits if the buffer is invalid. */
static void
check_user_buffer (const void *buf, unsigned size, bool writable)
{
  if (size == 0)
    return;
  const uint8_t *p = buf;
  if (!is_user_vaddr (p) || size > (uintptr_t) PHYS_BASE - (uintptr_t) p)
    sys_exit (-1);
  const uint8_t *end = p + size;
  while (p < end)
    {
      int b = get_user (p);
      if (b == -1)
        sys_exit (-1);
      if (writable && !put_user ((uint8_t *) p, b)) // Check if writing to user memory fails
        sys_exit (-1);
      p = (const uint8_t *) pg_round_down (p) + PGSIZE;
    }
}

static void
syscall_handler (struct intr_frame *f) 
{
  // validate_user_ptr (f->esp, sizeof(uint32_t));

  if (!is_user_vaddr (f->esp))
    sys_exit (-1);
  check_user_buffer (f->esp, sizeof(uint32_t), false);

  int syscall_num = *(int *)f->esp;

  switch (syscall_num) 
  {
    case SYS_EXEC:
      f->eax = sys_exec ((const char *) ARG(1)); 
      break;
    case SYS_EXIT:
      sys_exit ((int) ARG(1)); 
      break;
    case SYS_WRITE:
      {
        int fd = (int) ARG(1);
        const void *buffer = (const void *) ARG(2);
        unsigned size = (unsigned) ARG(3);
        f->eax = sys_write (fd, buffer, size); 
        break;
      }
    case SYS_HALT:
      sys_halt ();
      break;
    case SYS_WAIT:
      f->eax = process_wait ((tid_t) ARG(1));
      break;
    case SYS_READ:
      {
        int fd = (int) ARG(1);
        void *buffer = (void *) ARG(2);
        unsigned size = (unsigned) ARG(3);
        f->eax = sys_read (fd, buffer, size);
        break;
      }
    case SYS_CREATE:
      f->eax = sys_create ((const char *) ARG(1), (unsigned) ARG(2));
      break;
    case SYS_OPEN:
      f->eax = sys_open ((const char *) ARG(1));
      break;
    case SYS_FILESIZE:
      f->eax = sys_filesize ((int) ARG(1));
      break;
    case SYS_SEEK:
      sys_seek ((int) ARG(1), (unsigned) ARG(2));
      break;
    case SYS_TELL:
      f->eax = sys_tell ((int) ARG(1));
      break;
    case SYS_CLOSE:
      sys_close ((int) ARG(1));
      break;
    default:
      sys_exit (-1);
  }
}

static int sys_exec (const char *cmd)
{
  check_user_string (cmd);
  return process_execute (cmd);
}

void sys_exit (int status) 
{
  struct thread *cur = thread_current ();
  cur->exit_status = status;
  // printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

static void sys_halt (void)
{
  shutdown_power_off ();
}


static bool sys_create (const char *name, unsigned size)
{
  check_user_string (name);
  lock_acquire (&filesys_lock);
  bool ok = filesys_create (name, size);
  lock_release (&filesys_lock);
  return ok;
}

static struct file *get_file (int fd)
{
  if (fd < 2 || fd >= MAX_FD)
    return NULL;
  return thread_current ()->fd_table[fd];
}

static int sys_open (const char *name)
{
  check_user_string (name);
  lock_acquire (&filesys_lock);
  struct file *f = filesys_open (name);
  lock_release (&filesys_lock);
  if (f == NULL)
    return -1;
  struct thread *cur = thread_current ();
  for (int fd = 2; fd < MAX_FD; fd++)
    if (cur->fd_table[fd] == NULL)
      {
        cur->fd_table[fd] = f;
        return fd;
      }
  lock_acquire (&filesys_lock);
  file_close (f);
  lock_release (&filesys_lock);
  return -1;
}

static int sys_filesize (int fd)
{
  struct file *f = get_file (fd);
  if (f == NULL) return -1;
  lock_acquire (&filesys_lock);
  int n = file_length (f);
  lock_release (&filesys_lock);
  return n;
}

static int sys_read (int fd, void *buf, unsigned size)
{
  check_user_buffer (buf, size, true);
  if (fd == STDIN_FILENO)
    {
      uint8_t *p = buf;
      for (unsigned i = 0; i < size; i++)
        p[i] = input_getc ();
      return size;
    }
  struct file *f = get_file (fd);
  if (f == NULL) return -1;  
  lock_acquire (&filesys_lock);
  int n = file_read (f, buf, size);
  lock_release (&filesys_lock);
  return n;
}

static int sys_write (int fd, const void *buf, unsigned size)
{
  check_user_buffer (buf, size, false);
  if (fd == STDOUT_FILENO)
    {
      const char *p = buf;
      unsigned left = size;
      while (left > 0)
        {
          unsigned n = left < 512 ? left : 512;
          putbuf (p, n);
          p += n; left -= n;
        }
      return size;
    }
  struct file *f = get_file (fd);
  if (f == NULL) return -1; 
  lock_acquire (&filesys_lock);
  int n = file_write (f, buf, size);
  lock_release (&filesys_lock);
  return n;
}

static void sys_seek (int fd, unsigned pos)
{
  struct file *f = get_file (fd);
  if (f == NULL) return;
  if ((int) pos < 0) pos = INT_MAX;
  lock_acquire (&filesys_lock);
  file_seek (f, pos);
  lock_release (&filesys_lock);
}

static unsigned sys_tell (int fd)
{
  struct file *f = get_file (fd);
  if (f == NULL) return (unsigned) -1;
  lock_acquire (&filesys_lock);
  unsigned n = file_tell (f);
  lock_release (&filesys_lock);
  return n;
}

static void sys_close (int fd)
{
  struct file *f = get_file (fd);
  if (f == NULL) return; 
  lock_acquire (&filesys_lock);
  file_close (f);
  lock_release (&filesys_lock);
  thread_current ()->fd_table[fd] = NULL;
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault occurred. */
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