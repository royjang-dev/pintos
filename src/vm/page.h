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

enum page_kind { PAGE_FILE, PAGE_ZERO };

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
    struct lock lock;           /* Held while this page is being loaded/evicted. */
    struct hash_elem elem;
  };

bool spt_init (struct hash *spt);
void spt_destroy (struct hash *spt);
struct page *spt_find (struct hash *spt, void *upage);
bool spt_add_file_page (void *upage, struct file *file, off_t ofs,
                        uint32_t read_bytes, uint32_t zero_bytes, bool writable);
bool spt_add_zero_page (void *upage, bool writable);
bool page_fault_load (void *fault_addr, bool write);

#endif /* VM_PAGE_H */