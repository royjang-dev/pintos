#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"

static thread_func start_process NO_RETURN;
static bool load (const char *file_name, int argc, char **argv, void (**eip) (void), void **esp);
static void release_child (struct child_info *cp);
static void load_failed (struct thread *cur, char *page) NO_RETURN; 

#define MAX_ARGS 128

/* Passed from process_execute() to start_process(). */
struct exec_args
{
  char *cmdline; 
  struct child_info *cp;
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  if (file_name == NULL || strlen (file_name) > PGSIZE)  //Checks file_name, if it's nullptr or bigger than 4kb
    return TID_ERROR;

  char name[16]; // Buffer for storing the extracted program name
  int i = 0;
  while (i < 15 && file_name[i] != '\0' && file_name[i] != ' ') {
    name[i] = file_name[i];
    i++;
  }
  name[i] = '\0';

  char *fn_copy;
  struct exec_args *args;
  struct child_info *cp;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(PAL_ZERO);
  if (fn_copy == NULL)
    return TID_ERROR;

  strlcpy (fn_copy, file_name, PGSIZE);

  args = malloc (sizeof *args);
  cp = malloc (sizeof *cp);
  if (args == NULL || cp == NULL)
    {
      free (args);
      free (cp);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  cp->exit_status = -1;
  cp->is_exited = false;
  cp->is_orphan = false;
  cp->load_success = false;
  sema_init (&cp->wait_sema, 0);
  sema_init (&cp->load_sema, 0);
  args->cmdline = fn_copy;
  args->cp = cp;

  /* Create a new thread to execute with only the first word in file_name aka cmd. */
  tid = thread_create (name, PRI_DEFAULT, start_process, args); 

  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      free (args);
      free (cp);
      return TID_ERROR;
    }
  cp->tid = tid;
  list_push_back (&thread_current ()->child_list, &cp->child_elem);

  sema_down (&cp->load_sema);
  if (!cp->load_success)
  {
    list_remove (&cp->child_elem);
    release_child (cp);
    return TID_ERROR;
  }

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args_)
{
  struct exec_args *args = args_;
  char *file_name = args->cmdline;
  struct thread *cur = thread_current ();
  struct intr_frame if_;
  bool success;

  cur->child_self = args->cp;
  free (args);

  char *argv[MAX_ARGS];
  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r (file_name, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
  {
    if (argc == MAX_ARGS) {
      load_failed (cur, file_name);
    }
    argv[argc++] = token;
  }
  
  if (argc == 0) {
    load_failed (cur, file_name);
  }

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  // success = load (file_name, &if_.eip, &if_.esp);
  success = load (argv[0], argc, argv, &if_.eip, &if_.esp); 

  /* If load failed, quit. */
  // palloc_free_page (file_name);
  cur->exit_status = success ? 0 : -1;

  if (!success) {
    load_failed (cur, file_name);
  }
  else {
    palloc_free_page (file_name);
    if (cur->child_self != NULL) {
      cur->child_self->load_success = true;
      sema_up (&cur->child_self->load_sema);
    }
  }
  
  // sema_up (&cur->child_self->load_sema);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  #ifdef USERPROG
  struct thread *cur = thread_current ();
  struct child_info *child = NULL;
  struct list_elem *e;

  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list); e = list_next (e)) 
    {
      struct child_info *cp = list_entry (e, struct child_info, child_elem);
      if (cp->tid == child_tid) 
        {
          child = cp;
          break;
        }
    }

  if (child == NULL) 
    return -1;

  sema_down (&child->wait_sema);

  int status = child->exit_status;

  list_remove (&child->child_elem);
  free (child);
  return status;
  #else
    return -1;
  
  #endif
}


void
process_exit (void)
{
  struct thread *cur = thread_current ();
  if (cur->child_self != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  lock_acquire (&filesys_lock);
  for (int fd = 2; fd < 128; fd++)
  {
    if (cur->fd_table[fd] != NULL)
    {
      file_close (cur->fd_table[fd]);
      cur->fd_table[fd] = NULL;
    }
  }
  if (cur->exec_file != NULL)
  {
    file_close (cur->exec_file);
    cur->exec_file = NULL;
  }
  lock_release (&filesys_lock);

  struct list_elem *e = list_begin (&cur->child_list);
  while (e != list_end (&cur->child_list))
  {
    struct child_info *c = list_entry (e, struct child_info, child_elem);
    e = list_remove (e);
    release_child (c);
  }

  #ifdef USERPROG
  uint32_t *pd = cur->pagedir;
  if (pd != NULL) 
  {
    cur->pagedir = NULL;
    pagedir_activate (NULL);
    pagedir_destroy (pd);
  }
  #endif

  if (cur->child_self != NULL)
  {
    struct child_info *cp = cur->child_self;
    enum intr_level old = intr_disable ();
    
    cp->exit_status = cur->exit_status;
    cp->is_exited = true;
    bool orphan = cp->is_orphan;
    
    sema_up (&cp->wait_sema); 
    intr_set_level (old);
    
    if (orphan)
      free (cp);
      
    cur->child_self = NULL;
  }
}


/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, int argc, char **argv);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, int argc, char **argv, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  lock_acquire (&filesys_lock);

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, argc, argv))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  if (success) {
    file_deny_write (file); // Deny write access to the executable file after successful load
    t->exec_file = file;
  }
  else
    file_close (file);

  /* We arrive here whether the load is successful or not. */
  lock_release (&filesys_lock);
  return success;
}

static void
load_failed (struct thread *cur, char *page)
{
  palloc_free_page (page);
  cur->exit_status = -1;
  cur->child_self->load_success = false;
  sema_up (&cur->child_self->load_sema);
  thread_exit ();
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, int argc, char **argv)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success) {
        *esp = PHYS_BASE;
        uint8_t *sp = (uint8_t *) *esp;

        for (int i = argc - 1; i >= 0; i--)
        {
          size_t arg_len = strlen(argv[i]) + 1;

          if (sp - arg_len < (uint8_t *) PHYS_BASE - PGSIZE)
            return false;

          sp -= arg_len;
          memcpy(sp, argv[i], arg_len);
          argv[i] = (char *) sp; 
        }

        sp = (uint8_t *)((uintptr_t)sp & ~3);

        size_t meta_size = (argc + 1) * sizeof (char *) + 3 * sizeof (uint32_t);
        if (sp - meta_size < (uint8_t *) PHYS_BASE - PGSIZE)
          return false;

        char *null_ptr = NULL;
        if (sp - sizeof(null_ptr) < (uint8_t *) PHYS_BASE - PGSIZE)
          return false;
        sp -= sizeof(null_ptr);
        memcpy(sp, &null_ptr, sizeof(null_ptr));

        for (int i = argc - 1; i >= 0; i--)
          {
            if (sp - sizeof(argv[i]) < (uint8_t *) PHYS_BASE - PGSIZE)
              return false;
            sp -= sizeof(argv[i]);
            memcpy(sp, &argv[i], sizeof(argv[i]));
          }

        char **argv_user = (char **) sp;
        if (sp - sizeof(argv_user) < (uint8_t *) PHYS_BASE - PGSIZE)
          return false;
        sp -= sizeof(argv_user);
        memcpy(sp, &argv_user, sizeof(argv_user));

        if (sp - sizeof(argc) < (uint8_t *) PHYS_BASE - PGSIZE)
          return false;
        sp -= sizeof(argc);
        memcpy(sp, &argc, sizeof(argc));

        uint32_t fake_return = 0;
        if (sp - sizeof(fake_return) < (uint8_t *) PHYS_BASE - PGSIZE)
          return false;
        sp -= sizeof(fake_return);
        memcpy(sp, &fake_return, sizeof(fake_return));

        *esp = sp;
      }
      else
        palloc_free_page (kpage);
    }
  return success;
}


/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Releases a child_info structure.
   If the child has exited, it is freed.
   Otherwise, it is marked as orphan. */
static void
release_child (struct child_info *cp)
{
  enum intr_level old = intr_disable ();
  bool exited = cp->is_exited;
  if (!exited)
    cp->is_orphan = true;
  intr_set_level (old);
  if (exited)
    free (cp);
}