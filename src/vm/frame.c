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

struct list vm_frames;  // Danh sách các frame

// Khóa để đồng bộ hóa truy cập vào bảng frame
static struct lock vm_lock;

// Khóa để đảm bảo quá trình eviction là nguyên tử
static struct lock eviction_lock;

// Thêm một frame mới vào danh sách vm_frames
static bool add_vm_frame (void *);

// Loại bỏ một frame khỏi danh sách vm_frames
static void remove_vm_frame (void *);

// Lấy struct vm_frame tương ứng với một frame cụ thể từ danh sách vm_frames
static struct vm_frame *get_vm_frame (void *);

// Chọn một frame để loại bỏ (evict)
static struct vm_frame *frame_to_evict (void); 

// Lưu nội dung của frame đã bị loại bỏ vào swap hoặc file
static bool save_evicted_frame (struct vm_frame *);

/* Khởi tạo bảng frame và các cấu trúc dữ liệu cần thiết */
void vm_frame_init () {
  list_init (&vm_frames); // Khởi tạo danh sách liên kết vm_frames
  lock_init (&vm_lock);   // Khởi tạo khóa đồng bộ hóa vm_lock
  lock_init (&eviction_lock); // Khởi tạo khóa eviction_lock
}

// Cấp phát một frame từ vùng nhớ người dùng
void *vm_allocate_frame(enum palloc_flags flags) {
  void *frame = NULL;

  // Cố gắng cấp phát một trang từ vùng nhớ người dùng (USER_POOL)
  if (flags & PAL_USER)
    {
      if (flags & PAL_ZERO)
        frame = palloc_get_page (PAL_USER | PAL_ZERO);
      else
        frame = palloc_get_page (PAL_USER);
    }

  // Nếu cấp phát thành công, thêm frame vào danh sách vm_frames
  if (frame != NULL)
    add_vm_frame (frame);
  else  // Nếu không còn frame trống, thực hiện eviction để lấy một frame
    if ((frame = evict_frame ()) == NULL)
      PANIC ("Evicting frame failed");  // Nếu eviction thất bại, báo lỗi

  return frame;
}

// Giải phóng một frame
void vm_free_frame (void *frame) {
  remove_vm_frame (frame); // Loại bỏ frame khỏi danh sách vm_frames
  palloc_free_page (frame); // Giải phóng vùng nhớ của frame
}

// Thiết lập thông tin người dùng cho một frame
void frame_set_usr (void *frame, uint32_t *pte, void *upage) {
  struct vm_frame *vf;
  vf = get_vm_frame (frame); // Lấy struct vm_frame tương ứng với frame
  if (vf != NULL)
    {
      vf->page_table_entry = pte;   // Lưu trữ con trỏ tới mục nhập bảng trang
      vf->user_virtual_address = upage; // Lưu trữ địa chỉ ảo
    }
}

// Chọn một frame để loại bỏ (evict)
void * evict_frame () {
  bool result;
  struct vm_frame *vf;
  struct thread *t = thread_current (); // Lấy luồng hiện tại

  lock_acquire (&eviction_lock); // Khóa để đảm bảo quá trình eviction là nguyên tử

  vf = frame_to_evict ();  // Chọn frame để evict
  if (vf == NULL)
    PANIC ("No frame to evict.");  // Nếu không có frame nào để evict, báo lỗi

  result = save_evicted_frame (vf);   // Lưu nội dung của frame đã bị evict
  if (!result)
    PANIC ("can't save evicted frame");  // Nếu lưu thất bại, báo lỗi
  
  // Cập nhật thông tin của frame đã bị evict
  vf->thread_id = t->tid;
  vf->page_table_entry = NULL;
  vf->user_virtual_address = NULL;

  lock_release (&eviction_lock);  // Mở khóa eviction_lock

  return vf->frame;  // Trả về con trỏ tới frame đã bị evict
}

// Hàm chọn 1 frame để đuổi
static struct vm_frame *frame_to_evict () {
  struct vm_frame *vf;
  struct thread *t;
  struct list_elem *e;

  struct vm_frame *vf_class0 = NULL;

  int round_count = 1;
  bool found = false;
  // Duyệt qua từng mục trong bảng frame
  while (!found)
    {
      /* Duyệt qua danh sách frame, cố gắng tìm frame thuộc lớp (0,0).
        Nếu tìm thấy, kết thúc việc chọn frame để đuổi. Nếu không, đặt lại bit accessed 
        của mỗi trang về 0.
        Tối đa 2 vòng lặp, nếu vẫn không tìm thấy frame (0,0), ta phải chọn frame đầu tiên
        thuộc lớp khác 0. */
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
 
/* Lưu nội dung của frame đã bị đuổi (evict) vào swap hoặc file */
static bool save_evicted_frame (struct vm_frame *vf) {
  struct thread *t;
  struct suppl_pte *spte;

  /* Lấy thread tương ứng với tid của vm_frame và bảng trang bổ sung của nó */
  t = thread_get_by_id (vf->thread_id);

 /* Lấy mục nhập bảng trang bổ sung tương ứng với địa chỉ ảo của vm_frame */
  spte = get_suppl_pte (&t->suppl_page_table, vf->user_virtual_address);

  /* Nếu không tìm thấy mục nhập bảng trang bổ sung, tạo một mục mới và chèn vào bảng */
  if (spte == NULL)
    {
      spte = calloc(1, sizeof *spte);
      spte->uvaddr = vf->user_virtual_address;
      spte->type = SWAP;
      if (!insert_suppl_pte (&t->suppl_page_table, spte))
        return false;
    }

  size_t swap_slot_idx;
  /* Nếu trang bị bẩn và là mmf_page, ghi lại vào file 
  * Nếu không, nếu trang bị bẩn, đưa vào swap 
  * Nếu trang không bẩn và không phải file, thì đó là stack, cần đưa vào swap 
  * Ở đây, đối với file không bẩn, không làm gì cả, vì luôn có thể tải lại file khi cần. */
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
  /* Nếu trang sạch hoặc chỉ đọc, không làm gì cả */

  memset (vf->frame, 0, PGSIZE);
  /* cập nhật các thuộc tính swap, bao gồm swap_slot_idx,
     và swap_writable */
  spte->swap_slot_idx = swap_slot_idx;
  spte->swap_writable = *(vf->page_table_entry) & PTE_W;

  spte->is_loaded = false;

  /* bỏ ánh xạ khỏi pagedir của người dùng, giải phóng trang vm/khung */
  pagedir_clear_page (t->pagedir, spte->uvaddr);

  return true;
}

/* Thêm một mục mới (entry) vào bảng quản lý frame*/
static bool add_vm_frame (void *frame) {
  struct vm_frame *vf;
  vf = calloc (1, sizeof *vf);  // Cấp phát bộ nhớ cho một cấu trúc vm_frame mới
 
  if (vf == NULL)   // Nếu cấp phát bộ nhớ thất bại, trả về false
    return false;

  vf->thread_id = thread_current ()->tid;  // Gán ID của luồng hiện tại cho frame
  vf->frame = frame;  // Gán địa chỉ của frame vào cấu trúc vm_frame
  
  lock_acquire (&vm_lock);  // Khóa để đảm bảo an toàn luồng
  list_push_back (&vm_frames, &vf->elem);  // Thêm frame vào cuối danh sách vm_frames
  lock_release (&vm_lock);

  return true;
}

/* Loại bỏ một mục (entry) khỏi bảng quản lý frame (frame table) và giải phóng vùng nhớ được cấp phát cho mục đó */
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

/* Tìm và trả về cấu trúc vm_frame tương ứng với một frame cụ thể trong danh sách vm_frames*/
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
