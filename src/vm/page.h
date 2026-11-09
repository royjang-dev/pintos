#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "filesys/off_t.h"
#include "threads/synch.h"

struct file;
struct frame;
struct thread;

#define SWAP_NONE ((size_t) -1)

enum page_kind { PAGE_FILE, PAGE_ZERO, PAGE_MMAP };

struct page
  {
    void *upage;
    enum page_kind kind;
    struct file *file;  
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    bool writable;
    struct thread *owner;
    struct frame *frame;
    size_t swap_slot;
    bool pinned;                /* Set under lock; eviction skips pinned pages. */
    struct lock lock;           /* Held while this page is being loaded/evicted. */
    struct hash_elem elem;
  };

bool spt_init (struct hash *spt);
void spt_destroy (struct hash *spt);
struct page *spt_find (struct hash *spt, void *upage);
bool spt_add_file_page (void *upage, struct file *file, off_t ofs,
                        uint32_t read_bytes, uint32_t zero_bytes, bool writable);
bool spt_add_zero_page (void *upage, bool writable);
bool page_fault_load (void *fault_addr, bool write, void *esp);
bool page_pin_range (const void *buf, size_t size, bool write);
void page_unpin_range (const void *buf, size_t size);
void page_write_back (struct page *p, const void *kpage);
int mmap_create (struct file *file, void *addr, off_t length);
bool mmap_destroy (int mapid);
void mmap_destroy_all (void);

#endif /* VM_PAGE_H */