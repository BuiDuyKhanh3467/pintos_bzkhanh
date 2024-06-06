#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

/* Cấu trúc dữ liệu biểu diễn một frame trong bộ nhớ vật lý */
struct vm_frame {
  void *frame;       // Con trỏ tới địa chỉ bắt đầu của frame trong bộ nhớ vật lý
  tid_t thread_id;         // ID của tiến trình (thread) sở hữu frame này
  uint32_t *page_table_entry;     // Con trỏ tới mục nhập trong bảng trang (page table) ánh xạ tới frame này
  void *user_virtual_address;         // Địa chỉ ảo (user virtual address) ánh xạ tới frame này
  struct list_elem elem; // Phần tử liên kết để đưa frame vào danh sách vm_frames
};

/* Danh sách liên kết chứa tất cả các frame */
extern struct list vm_frames;

/* Hàm khởi tạo frame */
void vm_frame_init (void);

/* Hàm cấp phát frame */
void *vm_allocate_frame(enum palloc_flags flags);

/* Hàm cấp phát giải phóng frame */
void vm_free_frame (void *);

/* Hàm thiết lập thông tin người dùng cho một frame */
void frame_set_usr (void*, uint32_t *, void *);

/* Hàm loại bỏ một frame khỏi bộ nhớ vật lý và lưu nội dung của nó vào swap */
void *evict_frame (void);

#endif /* vm/frame.h */
