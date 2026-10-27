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
#include "threads/malloc.h"

#define FD_MAX 128
#define ARG(n) read_user_word ((uint32_t *) f->esp + (n))

static void syscall_handler (struct intr_frame *);

static struct fd_entry *get_entry (int fd);
static struct file *get_file (int fd);
static int fd_find_free (struct thread *t, int start);
static void fd_entry_close(struct fd_entry *e);
// void validate_user_ptr (const void *uaddr, size_t size);
static int sys_write (int fd, const void *buffer, unsigned size);
static int sys_read (int fd, void *buffer, unsigned size);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static int sys_exec (const char *cmd_line);
static void sys_exit (int status);
static void sys_close (int fd);
static void sys_halt (void);
static int sys_pipe (int pipefd[2]);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static int pipe_read (struct pipe *p, uint8_t *dst, unsigned size);
static int pipe_write (struct pipe *p, const uint8_t *src, unsigned size);

struct lock filesys_lock;  /* Lock for synchronizing access to the file system. */

enum fd_type { FD_FILE, FD_PIPE_R, FD_PIPE_W };

struct fd_entry
  {
    enum fd_type type;
    struct file *file;          /* FD_FILE only. */
    struct pipe *pipe;          /* FD_PIPE_R / FD_PIPE_W only. */
  };

#define PIPE_CAPACITY 512 

struct pipe
  {
    uint8_t buf[PIPE_CAPACITY];
    size_t head;     
    size_t count;          
    int readers;           
    int writers; 
    struct lock lock;
    struct condition not_empty;
    struct condition not_full;
  };

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
    case SYS_REMOVE:
      f->eax = sys_remove ((const char *) ARG(1));
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
    case SYS_PIPE:
      f->eax = sys_pipe ((int *) ARG(1));
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

static bool sys_remove (const char *name)
{
  check_user_string (name);
  lock_acquire (&filesys_lock);
  bool ok = filesys_remove (name);
  lock_release (&filesys_lock);
  return ok;
}

// static struct file *get_file (int fd)
// {
//   if (fd < 2 || fd >= FD_MAX)
//     return NULL;
//   return thread_current ()->fd_table[fd];
// }

static int
sys_open (const char *name)
{
  check_user_string (name);
  lock_acquire (&filesys_lock);
  struct file *f = filesys_open (name);
  lock_release (&filesys_lock);
  if (f == NULL)
    return -1;

  struct thread *cur = thread_current ();
  struct fd_entry *e = malloc (sizeof *e);
  int fd = fd_find_free (cur, 2);
  if (e == NULL || fd < 0)
    {
      free (e);
      lock_acquire (&filesys_lock);
      file_close (f);
      lock_release (&filesys_lock);
      return -1;
    }
  e->type = FD_FILE;  e->file = f;  e->pipe = NULL;
  cur->fd_table[fd] = e;
  return fd;
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

static int
sys_read (int fd, void *buf, unsigned size)
{
  check_user_buffer (buf, size, true);
  struct fd_entry *e = get_entry (fd);
  if (e == NULL)
    {
      if (fd != STDIN_FILENO)
        return -1;
      uint8_t *p = buf;
      for (unsigned i = 0; i < size; i++)
        p[i] = input_getc ();
      return size;
    }
  switch (e->type)
    {
    case FD_PIPE_R:
      return pipe_read (e->pipe, buf, size);
    case FD_FILE:
      {
        lock_acquire (&filesys_lock);
        int n = file_read (e->file, buf, size);
        lock_release (&filesys_lock);
        return n;
      }
    default:  
      return -1;
    }
}

/* Writes SIZE bytes from BUF to the file descriptor FD.
   Returns the number of bytes written, or -1 on error. */
static int sys_write (int fd, const void *buf, unsigned size)
{
  check_user_buffer (buf, size, false);
  struct fd_entry *e = get_entry (fd);
  /* console output for STDOUT_FILENO if fd=1 and no entry exists in fd_table */
  if (fd == STDOUT_FILENO && e == NULL) 
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

  if (e == NULL) {
    return -1; // Entry not found and not STDOUT_FILENO
  }

  switch (e->type)
    {
    case FD_PIPE_W:
      return pipe_write (e->pipe, buf, size);

    case FD_FILE:
      {
        lock_acquire (&filesys_lock);
        int n = file_write (e->file, buf, size);
        lock_release (&filesys_lock);
        return n;
      }

    default:  
      return -1;
    }
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

static void
sys_close (int fd)
{
  struct fd_entry *e = get_entry (fd);
  if (e == NULL)
    return;  
  thread_current ()->fd_table[fd] = NULL;
  fd_entry_close (e);
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

/* Pipe implementation. */
static struct pipe *
pipe_create (void)
{
  struct pipe *p = malloc (sizeof *p);
  if (p == NULL)
    return NULL;
  p->head = p->count = 0;
  p->readers = p->writers = 1; 
  lock_init (&p->lock);
  cond_init (&p->not_empty);
  cond_init (&p->not_full);
  return p;
}

void
pipe_add_reader (struct pipe *p)
{
  lock_acquire (&p->lock);
  p->readers++;
  lock_release (&p->lock);
}

static void
pipe_close_end (struct pipe *p, bool reader)
{
  lock_acquire (&p->lock);
  if (reader)
    p->readers--;
  else
    p->writers--;
  bool last = p->readers == 0 && p->writers == 0;
  cond_broadcast (&p->not_empty, &p->lock); 
  cond_broadcast (&p->not_full, &p->lock); 
  lock_release (&p->lock);
  if (last)
    free (p);    
}

void
pipe_drop_reader (struct pipe *p)
{
  pipe_close_end (p, true);
}

static int
pipe_read (struct pipe *p, uint8_t *dst, unsigned size)
{
  if (size == 0)
    return 0;
  lock_acquire (&p->lock);
  while (p->count == 0 && p->writers > 0)
    cond_wait (&p->not_empty, &p->lock);
  unsigned n = size < p->count ? size : p->count;
  for (unsigned i = 0; i < n; i++)
    {
      dst[i] = p->buf[p->head];
      p->head = (p->head + 1) % PIPE_CAPACITY;
    }
  p->count -= n;
  if (n > 0)
    cond_broadcast (&p->not_full, &p->lock);
  lock_release (&p->lock);
  return n;
}

static int
pipe_write (struct pipe *p, const uint8_t *src, unsigned size)
{
  lock_acquire (&p->lock);
  if (p->readers == 0)
    {
      lock_release (&p->lock);
      return -1;
    }
  unsigned done = 0;
  while (done < size)
    {
      if (p->readers == 0)  
        {
          lock_release (&p->lock);
          return -1;
        }
      if (p->count == PIPE_CAPACITY)
        {
          cond_wait (&p->not_full, &p->lock);
          continue;
        }
      while (done < size && p->count < PIPE_CAPACITY)
        {
          p->buf[(p->head + p->count) % PIPE_CAPACITY] = src[done++];
          p->count++;
        }
      cond_broadcast (&p->not_empty, &p->lock);
    }
  lock_release (&p->lock);
  return size;
}

/* Returns the fd_entry corresponding to fd, or NULL if invalid. */
static struct fd_entry *
get_entry (int fd)
{
  if (fd < 0 || fd >= FD_MAX)
    return NULL;
  return thread_current ()->fd_table[fd];
}

static struct file *
get_file (int fd)   
{
  struct fd_entry *e = get_entry (fd);
  return (e != NULL && e->type == FD_FILE) ? e->file : NULL;
}

static int
fd_find_free (struct thread *t, int start)
{
  for (int fd = start; fd < FD_MAX; fd++)
    if (t->fd_table[fd] == NULL)
      return fd;
  return -1;
}

static void
fd_entry_close (struct fd_entry *e)
{
  switch (e->type)
    {
    case FD_FILE:
      lock_acquire (&filesys_lock);
      file_close (e->file);
      lock_release (&filesys_lock);
      break;
    case FD_PIPE_R:
      pipe_close_end (e->pipe, true);
      break;
    case FD_PIPE_W:
      pipe_close_end (e->pipe, false);
      break;
    }
  free (e);
}

void
fd_table_close_all (struct thread *t)
{
  for (int fd = 0; fd < FD_MAX; fd++)
    if (t->fd_table[fd] != NULL)
      {
        struct fd_entry *e = t->fd_table[fd];
        t->fd_table[fd] = NULL;
        fd_entry_close (e);
      }
}

bool
fd_install_pipe_reader (struct thread *t, int fd, struct pipe *p)
{
  struct fd_entry *e = malloc (sizeof *e);
  if (e == NULL)
    {
      pipe_close_end (p, true); 
      return false;
    }
  e->type = FD_PIPE_R;
  e->file = NULL;
  e->pipe = p;
  t->fd_table[fd] = e;
  return true;
}

/* Finds a pipe in the current thread's fd table that has both read and write ends installed. Returns the pipe if found, or NULL otherwise. */
struct pipe *
pipe_find_pair (struct thread *t)
{
  for (int r = 0; r < FD_MAX; r++)
    {
      struct fd_entry *re = t->fd_table[r];
      if (re == NULL || re->type != FD_PIPE_R)
        continue;
      for (int w = 0; w < FD_MAX; w++)
        {
          struct fd_entry *we = t->fd_table[w];
          if (we != NULL && we->type == FD_PIPE_W && we->pipe == re->pipe)
            return re->pipe;
        }
    }
  return NULL;
}

/* Creates a pipe and installs its read and write ends in the current thread's fd table. */
static int
sys_pipe (int *fds)
{
  check_user_buffer (fds, 2 * sizeof (int), true);

  struct thread *cur = thread_current ();
  int rfd = fd_find_free (cur, 2);
  int wfd = rfd < 0 ? -1 : fd_find_free (cur, rfd + 1);
  if (wfd < 0)
    return -1;

  struct pipe *p = pipe_create ();
  struct fd_entry *re = malloc (sizeof *re);
  struct fd_entry *we = malloc (sizeof *we);
  if (p == NULL || re == NULL || we == NULL)
    {
      free (p);
      free (re);
      free (we);
      return -1;
    }
  re->type = FD_PIPE_R;  re->file = NULL;  re->pipe = p;
  we->type = FD_PIPE_W;  we->file = NULL;  we->pipe = p;
  cur->fd_table[rfd] = re;
  cur->fd_table[wfd] = we;

  fds[0] = rfd;   
  fds[1] = wfd;
  return 0;
}