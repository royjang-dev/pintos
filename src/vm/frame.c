#include "vm/frame.h"
#include <debug.h>
#include <stdint.h>
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"

static struct list frames;
static struct list_elem *hand;
static struct lock frame_lock; // Protects `frames` and `hand` only.

static void *frame_evict (struct page *p);

void frame_init (void)
{
    list_init (&frames);
    hand = NULL;
    lock_init (&frame_lock);
}

void *frame_alloc (struct page *p)
{
    void *kpage = palloc_get_page (PAL_USER);
    if (kpage == NULL)
        return frame_evict (p);
    struct frame *f = malloc (sizeof *f);
    if (f == NULL) { 
        palloc_free_page (kpage);
        return NULL; 
    }
    f->kpage = kpage;  f->page = p;  p->frame = f;
    lock_acquire (&frame_lock);  list_push_back (&frames, &f->elem);  lock_release (&frame_lock);
    return kpage;
}

/* Evict a frame to make room for page `p`. Returns the kernel page of the new frame. */
static void *frame_evict (struct page *p)
{
    lock_acquire (&frame_lock);
    struct frame *f = NULL;  
    struct page *v = NULL;
    size_t limit = 2 * list_size (&frames) + 2;

    while (limit-- > 0 && !list_empty (&frames))
    {
        if (hand == NULL || hand == list_end (&frames)) 
            hand = list_begin (&frames);
        f = list_entry (hand, struct frame, elem);
        hand = list_next (hand);
        v = f->page;
        if (!lock_try_acquire (&v->lock)) 
        { 
            v = NULL; 
            continue;
        }
        if (pagedir_is_accessed (v->owner->pagedir, v->upage))
        {
            pagedir_set_accessed (v->owner->pagedir, v->upage, false);
            lock_release (&v->lock);  
            v = NULL;
            continue;
        }
        break; // found a victim frame to evict
    }
    if (v == NULL) PANIC ("no evictable frame");

    uint32_t *pd = v->owner->pagedir;
    pagedir_clear_page (pd, v->upage);              /* the owner can no longer touch it */
    bool dirty = pagedir_is_dirty (pd, v->upage);   /* read AFTER clearing */
    f->page = p;  p->frame = f;  v->frame = NULL;   /* frame now belongs to the new page */
    lock_release (&frame_lock);                     /* <- everything below is slow I/O */

    if (dirty) v->swap_slot = swap_out (f->kpage);
    lock_release (&v->lock);
    return f->kpage;
}

void frame_free (struct page *p)
{
    struct frame *f = p->frame;
    lock_acquire (&frame_lock);
    if (hand == &f->elem) hand = list_next (hand);
    list_remove (&f->elem);
    lock_release (&frame_lock);
    palloc_free_page (f->kpage);  free (f);  p->frame = NULL;
}