#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

struct vm_frame {
  void *frame;       
  tid_t thread_id;      
  uint32_t *page_table_entry;    
  void *user_virtual_address;        
  struct list_elem elem; 
};

extern struct list vm_frames;

void vm_frame_init (void);

void *vm_allocate_frame(enum palloc_flags flags);

void vm_free_frame (void *);

void frame_set_usr (void*, uint32_t *, void *);

void *evict_frame (void);

#endif 
