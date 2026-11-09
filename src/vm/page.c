#include "vm/page.h"
#include <round.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/swap.h"

#define STACK_MAX (8 * 1024 * 1024)
#define STACK_SLACK 32

struct mmap_entry
{
  int id;
  struct file *file;
  void *addr;
  size_t page_cnt;
  struct list_elem elem;
};

static void destroy_page (struct hash_elem *e, void *aux);
static bool spt_add_page (enum page_kind kind, void *upage, struct file *file,
                          off_t ofs, uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static void page_release (struct page *p);

static bool is_stack_access (void *fault_addr, void *esp)
{
  uint8_t *a = fault_addr;
  if (esp == NULL || !is_user_vaddr (a))
    return false;
  if (a < (uint8_t *) PHYS_BASE - STACK_MAX)
    return false;
  return a + STACK_SLACK >= (uint8_t *) esp;
}

/* this function computes the hash value for a page based on its user virtual address (upage) */
static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct page *p = hash_entry (e, struct page, elem);
  return hash_bytes (&p->upage, sizeof p->upage);
}

/* Returns true if page a precedes page b in the hash table order. */
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  const struct page *pa = hash_entry (a, struct page, elem);
  const struct page *pb = hash_entry (b, struct page, elem);
  return pa->upage < pb->upage;
}

/* Initializes the supplemental page table. Returns true on success. */
bool spt_init (struct hash *spt)
{
  return hash_init (spt, page_hash, page_less, NULL);
}

/* Finds the page corresponding to the given user virtual address (upage) in the supplemental page table. Returns the page if found, or NULL if not found. */
struct page *spt_find (struct hash *spt, void *upage)
{
  struct page key;
  key.upage = upage;
  struct hash_elem *e = hash_find (spt, &key.elem);
  return e ? hash_entry (e, struct page, elem) : NULL;
}

/* Destroys the supplemental page table by freeing all pages and the hash table itself. */
void spt_destroy (struct hash *spt)
{
  hash_destroy (spt, destroy_page);
}

/* Adds a file-backed page to the supplemental page table. Returns true on success, false on failure. */
bool spt_add_file_page (void *upage, struct file *file, off_t ofs, uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  return spt_add_page (read_bytes == 0 ? PAGE_ZERO : PAGE_FILE, upage, file,
                       ofs, read_bytes, zero_bytes, writable);
}

static bool spt_add_page (enum page_kind kind, void *upage, struct file *file, off_t ofs, uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct thread *t = thread_current ();
  struct page *p = malloc (sizeof *p);
  if (p == NULL)
    return false;
  p->upage = upage;
  p->kind = kind;
  p->file = file;
  p->ofs = ofs;
  p->read_bytes = read_bytes;
  p->zero_bytes = zero_bytes;
  p->writable = writable;
  p->owner = t;
  p->frame = NULL;
  p->swap_slot = SWAP_NONE;
  p->pinned = false;
  lock_init (&p->lock);
  if (hash_insert (&t->spt, &p->elem) != NULL)
  {
    free (p);
    return false;
  }
  return true;
}

/* Adds a zero-initialized page to the supplemental page table. Returns true on success, false on failure. */
bool spt_add_zero_page (void *upage, bool writable)
{
  if (!spt_add_file_page (upage, NULL, 0, 0, PGSIZE, writable))
    return false; 
  return true;
}

/* Loads the given page into memory. Returns true on success, false on failure. */
static bool page_load (struct page *p)
{
  void *kpage = frame_alloc (p);
  if (kpage == NULL) 
    return false;
  bool from_swap = false;

  if (p->swap_slot != SWAP_NONE)
  {
    swap_in (p->swap_slot, kpage);
    p->swap_slot = SWAP_NONE;
    from_swap = true;
  }
  else if (p->kind == PAGE_FILE || p->kind == PAGE_MMAP)
  {
    bool held = lock_held_by_current_thread (&filesys_lock);
    if (!held) 
      lock_acquire (&filesys_lock);
    off_t n = file_read_at (p->file, kpage, p->read_bytes, p->ofs);
    if (!held)
      lock_release (&filesys_lock);
    if (n != (off_t) p->read_bytes)
    { 
      frame_free (p); return false; 
    }
    memset ((uint8_t *) kpage + p->read_bytes, 0, p->zero_bytes);
  }
  else
    memset (kpage, 0, PGSIZE);

  if (!pagedir_set_page (p->owner->pagedir, p->upage, kpage, p->writable))
  { 
    frame_free (p); 
    return false; 
  }
  if (from_swap)
    pagedir_set_dirty (p->owner->pagedir, p->upage, true);
  return true;
}

/* Makes the page of FAULT_ADDR resident (growing the stack if the access looks like one). If PIN, the page is marked pinned atomically with the load, under the page lock that eviction also takes. */
static bool fault_load (void *fault_addr, bool write, void *esp, bool pin)
{
  struct thread *t = thread_current ();
  if (!t->spt_ready)
    return false;
  void *upage = pg_round_down (fault_addr);
  struct page *p = spt_find (&t->spt, upage); // Look up the page in the supplemental page table
  if (p == NULL)
  {
    if (!is_stack_access (fault_addr, esp) || !spt_add_zero_page (upage, true))
      return false;
    p = spt_find (&t->spt, upage);
  }
  if (write && !p->writable)
    return false;
  lock_acquire (&p->lock);
  bool ok = p->frame != NULL || page_load (p); // Load the page if it is not already in a frame
  if (ok && pin)
    p->pinned = true;
  lock_release (&p->lock);
  return ok;
}

/* Handles a page fault by loading the page into memory, growing the stack if the access looks like one. ESP is the user stack pointer. Returns true on success, false on failure. */
bool page_fault_load (void *fault_addr, bool write, void *esp)
{
  return fault_load (fault_addr, write, esp, false);
}

static void unpin_pages (uint8_t *first, uint8_t *end)
{
  struct thread *t = thread_current ();
  for (uint8_t *pg = first; pg < end; pg += PGSIZE)
    {
      struct page *p = spt_find (&t->spt, pg);
      if (p != NULL)
        p->pinned = false;
    }
}

/* Makes every page of [BUF, BUF+SIZE) resident and unevictable. On failure nothing stays pinned. */
bool page_pin_range (const void *buf, size_t size, bool write)
{
  if (size == 0)
    return true;
  uint8_t *start = (uint8_t *) buf;
  if (start + size - 1 < start || !is_user_vaddr (start + size - 1))
    return false;

  uint8_t *first = pg_round_down (start);
  uint8_t *last = pg_round_down (start + size - 1);
  void *esp = thread_current ()->user_esp;
  for (uint8_t *pg = first; pg <= last; pg += PGSIZE)
    if (!fault_load (pg == first ? start : pg, write, esp, true))
      {
        unpin_pages (first, pg);
        return false;
      }
  return true;
}

void page_unpin_range (const void *buf, size_t size)
{
  if (size == 0)
    return;
  uint8_t *start = (uint8_t *) buf;
  unpin_pages (pg_round_down (start),
               (uint8_t *) pg_round_down (start + size - 1) + PGSIZE);
}

static void destroy_page (struct hash_elem *e, void *aux UNUSED)
{
  struct page *p = hash_entry (e, struct page, elem);
  page_release (p);
  free (p);
}

/* Writes the file-backed bytes of mmap page P from KPAGE back to its file. */
void page_write_back (struct page *p, const void *kpage)
{
  bool held = lock_held_by_current_thread (&filesys_lock);
  if (!held)
    lock_acquire (&filesys_lock);
  file_write_at (p->file, kpage, p->read_bytes, p->ofs);
  if (!held)
    lock_release (&filesys_lock);
}

/* Frees P's frame (writing back dirty mmap data) or swap slot, without freeing P itself. */
static void page_release (struct page *p)
{
  lock_acquire (&p->lock);                   /* waits if an evictor is writing it out */
  if (p->frame != NULL)
    {
      uint32_t *pd = p->owner->pagedir;
      if (p->kind == PAGE_MMAP && pagedir_is_dirty (pd, p->upage))
        page_write_back (p, p->frame->kpage);
      pagedir_clear_page (pd, p->upage);
      frame_free (p);
    }
  else if (p->swap_slot != SWAP_NONE)
    swap_free (p->swap_slot);
  lock_release (&p->lock);
}

/* Maps FILE (already reopened, LENGTH bytes) at ADDR. Takes ownership of FILE only on success. Returns the mapping id or -1. */
int mmap_create (struct file *file, void *addr, off_t length)
{
  struct thread *t = thread_current ();
  if (!t->spt_ready || addr == NULL || pg_ofs (addr) != 0 || length <= 0
      || !is_user_vaddr (addr))
    return -1;

  size_t cnt = DIV_ROUND_UP ((size_t) length, PGSIZE);
  if (cnt > ((uintptr_t) PHYS_BASE - (uintptr_t) addr) / PGSIZE)
    return -1;
  for (size_t i = 0; i < cnt; i++)
    if (spt_find (&t->spt, (uint8_t *) addr + i * PGSIZE) != NULL)
      return -1;

  struct mmap_entry *m = malloc (sizeof *m);
  if (m == NULL)
    return -1;

  size_t i;
  for (i = 0; i < cnt; i++)
    {
      off_t ofs = i * PGSIZE;
      uint32_t rb = length - ofs < PGSIZE ? length - ofs : PGSIZE;
      if (!spt_add_page (PAGE_MMAP, (uint8_t *) addr + ofs, file, ofs, rb,
                         PGSIZE - rb, true))
        break;
    }
  if (i < cnt)
    {
      while (i-- > 0)
        {
          struct page *p = spt_find (&t->spt, (uint8_t *) addr + i * PGSIZE);
          hash_delete (&t->spt, &p->elem);
          free (p);
        }
      free (m);
      return -1;
    }

  m->id = t->next_mapid++;
  m->file = file;
  m->addr = addr;
  m->page_cnt = cnt;
  list_push_back (&t->mmap_list, &m->elem);
  return m->id;
}

/* Writes back dirty pages of M, removes them from the SPT and closes the file. */
static void mmap_release (struct mmap_entry *m)
{
  struct thread *t = thread_current ();
  for (size_t i = 0; i < m->page_cnt; i++)
    {
      struct page *p = spt_find (&t->spt, (uint8_t *) m->addr + i * PGSIZE);
      page_release (p);
      hash_delete (&t->spt, &p->elem);
      free (p);
    }
  lock_acquire (&filesys_lock);
  file_close (m->file);
  lock_release (&filesys_lock);
  list_remove (&m->elem);
  free (m);
}

/* Unmaps MAPID. Returns false if the current process has no such mapping. */
bool mmap_destroy (int mapid)
{
  struct thread *t = thread_current ();
  for (struct list_elem *e = list_begin (&t->mmap_list);
       e != list_end (&t->mmap_list); e = list_next (e))
    {
      struct mmap_entry *m = list_entry (e, struct mmap_entry, elem);
      if (m->id == mapid)
        {
          mmap_release (m);
          return true;
        }
    }
  return false;
}

void mmap_destroy_all (void)
{
  struct thread *t = thread_current ();
  while (!list_empty (&t->mmap_list))
    mmap_release (list_entry (list_front (&t->mmap_list),
                              struct mmap_entry, elem));
}