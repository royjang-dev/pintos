#include "vm/page.h"
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

static void destroy_page (struct hash_elem *e, void *aux);

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
  struct thread *t = thread_current ();
  struct page *p = malloc (sizeof *p);
  if (p == NULL)
    return false;
  p->upage = upage;
  p->kind = read_bytes == 0 ? PAGE_ZERO : PAGE_FILE;
  p->file = file;
  p->ofs = ofs;
  p->read_bytes = read_bytes;
  p->zero_bytes = zero_bytes;
  p->writable = writable;
  p->owner = t;
  p->frame = NULL;
  p->swap_slot = SWAP_NONE;
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
  else if (p->kind == PAGE_FILE)
    {
      /* file_read_at, NOT file_seek+file_read: the file position is shared state. */
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
    { frame_free (p); return false; }
  if (from_swap)
    pagedir_set_dirty (p->owner->pagedir, p->upage, true);
  return true;
}

/* Handles a page fault by loading the page into memory. Returns true on success, false on failure. */
bool page_fault_load (void *fault_addr, bool write)
{
  struct thread *t = thread_current ();
  if (!t->spt_ready) return false;
  struct page *p = spt_find (&t->spt, pg_round_down (fault_addr));
  if (p == NULL) return false;               /* later: stack growth goes here */
  if (write && !p->writable) return false;
  lock_acquire (&p->lock);
  bool ok = p->frame != NULL || page_load (p);
  lock_release (&p->lock);
  return ok;
}

static void destroy_page (struct hash_elem *e, void *aux UNUSED)
{
  struct page *p = hash_entry (e, struct page, elem);
  lock_acquire (&p->lock);                   /* waits if an evictor is writing it out */
  if (p->frame != NULL)
    {
      pagedir_clear_page (p->owner->pagedir, p->upage);
      frame_free (p);
    }
  else if (p->swap_slot != SWAP_NONE)
    swap_free (p->swap_slot);
  lock_release (&p->lock);
  free (p);
}