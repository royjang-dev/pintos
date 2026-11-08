#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>
#include <stdint.h>

void swap_init (void);
size_t swap_out (const void *kpage);     /* returns the slot index */
void swap_in (size_t slot, void *kpage);
void swap_free (size_t slot);

#endif /* VM_SWAP_H */