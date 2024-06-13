#include "vm/page.h"
#include "vm/swap.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/pte.h"
#include "lib/kernel/list.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"

struct list vm_frames;  

static struct lock vm_lock;

static struct lock eviction_lock;

static bool add_vm_frame (void *);

static void remove_vm_frame (void *);

static struct vm_frame *get_vm_frame (void *);

static struct vm_frame *frame_to_evict (void); 

static bool save_evicted_frame (struct vm_frame *);

void vm_frame_init () {
  list_init (&vm_frames); 
  lock_init (&vm_lock);   
  lock_init (&eviction_lock); 
}

void *vm_allocate_frame(enum palloc_flags flags) {
  void *frame = NULL;

  if (flags & PAL_USER)
    {
      if (flags & PAL_ZERO)
        frame = palloc_get_page (PAL_USER | PAL_ZERO);
      else
        frame = palloc_get_page (PAL_USER);
    }

  if (frame != NULL)
    add_vm_frame (frame);
  else 
    if ((frame = evict_frame ()) == NULL)
      PANIC ("Evicting frame failed");  

  return frame;
}

void vm_free_frame (void *frame) {
  remove_vm_frame (frame); 
  palloc_free_page (frame); 
}

void frame_set_usr (void *frame, uint32_t *pte, void *upage) {
  struct vm_frame *vf;
  vf = get_vm_frame (frame); 
  if (vf != NULL)
    {
      vf->page_table_entry = pte;   
      vf->user_virtual_address = upage; 
    }
}

void * evict_frame () {
  bool result;
  struct vm_frame *vf;
  struct thread *t = thread_current (); 

  lock_acquire (&eviction_lock); 

  vf = frame_to_evict ();  
  if (vf == NULL)
    PANIC ("No frame to evict.");  

  result = save_evicted_frame (vf);   
  if (!result)
    PANIC ("can't save evicted frame");  
  
  vf->thread_id = t->tid;
  vf->page_table_entry = NULL;
  vf->user_virtual_address = NULL;

  lock_release (&eviction_lock);  

  return vf->frame;  
}

static struct vm_frame *frame_to_evict () {
  struct vm_frame *vf;
  struct thread *t;
  struct list_elem *e;

  struct vm_frame *vf_class0 = NULL;

  int round_count = 1;
  bool found = false;
  
  while (!found)
    {
      e = list_head (&vm_frames);
      while ((e = list_next (e)) != list_tail (&vm_frames))
        {
          vf = list_entry (e, struct vm_frame, elem);
          t = thread_get_by_id (vf->thread_id);
          bool accessed  = pagedir_is_accessed (t->pagedir, vf->user_virtual_address);
          if (!accessed)
            {
              vf_class0 = vf;
              list_remove (e);
              list_push_back (&vm_frames, e);
              break;
            }
          else
            {
              pagedir_set_accessed (t->pagedir, vf->user_virtual_address, false);
            }
        }

      if (vf_class0 != NULL)
        found = true;
      else if (round_count++ == 2)
        found = true;
    }

  return vf_class0;
}

static bool save_evicted_frame (struct vm_frame *vf) {
  struct thread *t;
  struct suppl_pte *spte;

  t = thread_get_by_id (vf->thread_id);

  spte = get_suppl_pte (&t->suppl_page_table, vf->user_virtual_address);

  if (spte == NULL)
    {
      spte = calloc(1, sizeof *spte);
      spte->uvaddr = vf->user_virtual_address;
      spte->type = SWAP;
      if (!insert_suppl_pte (&t->suppl_page_table, spte))
        return false;
    }

  size_t swap_slot_idx;

  if (pagedir_is_dirty (t->pagedir, spte->uvaddr)
      && (spte->type == MMF))
    {
      write_page_back_to_file_wo_lock (spte);
    }
  else if (pagedir_is_dirty (t->pagedir, spte->uvaddr)
           || (spte->type != FILE))
    {
      swap_slot_idx = vm_swap_out (spte->uvaddr);
      if (swap_slot_idx == SWAP_ERROR)
        return false;

      spte->type = spte->type | SWAP;
    }

  memset (vf->frame, 0, PGSIZE);

  spte->swap_slot_idx = swap_slot_idx;
  spte->swap_writable = *(vf->page_table_entry) & PTE_W;

  spte->is_loaded = false;

  pagedir_clear_page (t->pagedir, spte->uvaddr);

  return true;
}

static bool add_vm_frame (void *frame) {
  struct vm_frame *vf;
  vf = calloc (1, sizeof *vf);  
 
  if (vf == NULL)   
    return false;

  vf->thread_id = thread_current ()->tid;  
  vf->frame = frame;  
  
  lock_acquire (&vm_lock);  
  list_push_back (&vm_frames, &vf->elem);  
  lock_release (&vm_lock);

  return true;
}

static void remove_vm_frame (void *frame) {
  struct vm_frame *vf;
  struct list_elem *e;
  
  lock_acquire (&vm_lock);
  e = list_head (&vm_frames);
  while ((e = list_next (e)) != list_tail (&vm_frames))
    {
      vf = list_entry (e, struct vm_frame, elem);
      if (vf->frame == frame)
        {
          list_remove (e);
          free (vf);
          break;
        }
    }
  lock_release (&vm_lock);
}

static struct vm_frame *get_vm_frame (void *frame) {
  struct vm_frame *vf;
  struct list_elem *e;
  
  lock_acquire (&vm_lock);
  e = list_head (&vm_frames);
  while ((e = list_next (e)) != list_tail (&vm_frames))
    {
      vf = list_entry (e, struct vm_frame, elem);
      if (vf->frame == frame)
        break;
      vf = NULL;
    }
  lock_release (&vm_lock);

  return vf;
}
