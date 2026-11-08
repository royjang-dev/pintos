#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>

struct page;

struct frame
  {
    void *kpage;
    struct page *page; // page currently held in this frame
    struct list_elem elem;
  };

void frame_init (void);
void *frame_alloc (struct page *p);
void frame_free (struct page *p);

#endif