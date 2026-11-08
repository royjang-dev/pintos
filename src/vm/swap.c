#include "vm/swap.h"
#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)
static struct block *swap_dev;
static struct bitmap *swap_map;
static struct lock swap_lock;

void swap_init (void)
{
  swap_dev = block_get_role (BLOCK_SWAP);
  swap_map = bitmap_create (block_size (swap_dev) / SECTORS_PER_PAGE);
  lock_init (&swap_lock);
}

size_t swap_out (const void *kpage)
{
  lock_acquire (&swap_lock);
  size_t slot = bitmap_scan_and_flip (swap_map, 0, 1, false);
  lock_release (&swap_lock);
  if (slot == BITMAP_ERROR) PANIC ("swap full");
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
    block_write (swap_dev, slot * SECTORS_PER_PAGE + i,
                 (const uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
  return slot;
}

void swap_in (size_t slot, void *kpage)
{
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
    block_read (swap_dev, slot * SECTORS_PER_PAGE + i,
                (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
  swap_free (slot);
}

void swap_free (size_t slot)
{
  lock_acquire (&swap_lock);
  ASSERT (bitmap_test (swap_map, slot));
  bitmap_reset (swap_map, slot);
  lock_release (&swap_lock);
}