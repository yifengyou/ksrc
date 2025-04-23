# kfifo

## 简介

```text
include/linux/kfifo.h
lib/kfifo.c
samples/kfifo
```



```text
__kfifo
    in
    out
    mask
    esize
    data
kfifo
kfifo_rec_ptr_1
kfifo_rec_ptr_2


__kfifo_uint_must_check_helper
__kfifo_int_must_check_helper
__kfifo_alloc
__kfifo_free
__kfifo_init
__kfifo_in
__kfifo_out
__kfifo_from_user
__kfifo_to_user
__kfifo_dma_in_prepare
__kfifo_dma_out_prepare
__kfifo_out_peek
__kfifo_in_r
__kfifo_out_r
__kfifo_from_user_r
__kfifo_to_user_r
__kfifo_dma_in_prepare_r
__kfifo_dma_in_finish_r
__kfifo_dma_out_prepare_r
__kfifo_dma_out_finish_r
__kfifo_len_r
__kfifo_skip_r
__kfifo_out_peek_r
__kfifo_max_r
__must_check
__must_check


len
copied
len
copied
len
copied
size_t
len
copied
size_t


_LINUX_KFIFO_H
__STRUCT_KFIFO_COMMON
__STRUCT_KFIFO
STRUCT_KFIFO
__STRUCT_KFIFO_PTR
STRUCT_KFIFO_PTR
STRUCT_KFIFO_REC_1
STRUCT_KFIFO_REC_2
__is_kfifo_ptr
DECLARE_KFIFO_PTR
DECLARE_KFIFO
INIT_KFIFO
DEFINE_KFIFO
kfifo_initialized
kfifo_esize
kfifo_recsize
kfifo_size
kfifo_reset
kfifo_reset_out
kfifo_len
kfifo_is_empty
kfifo_is_full
kfifo_avail
kfifo_skip
kfifo_peek_len
kfifo_alloc
kfifo_free
kfifo_init
kfifo_put
kfifo_get
kfifo_peek
kfifo_in
kfifo_in_spinlocked
kfifo_in_locked
kfifo_out
kfifo_out_spinlocked
kfifo_out_locked
kfifo_from_user
kfifo_to_user
kfifo_dma_in_prepare
kfifo_dma_in_finish
kfifo_dma_out_prepare
kfifo_dma_out_finish
kfifo_out_peek
```


kfifo_alloc：分配一个新的FIFO缓冲区，并初始化它。
kfifo_free：释放一个FIFO缓冲区，并释放其内存。
kfifo_init：初始化一个已经分配的FIFO缓冲区。
kfifo_put：向FIFO缓冲区中放入一个元素。
kfifo_get：从FIFO缓冲区中取出一个元素。
kfifo_in：向FIFO缓冲区中放入多个元素。
kfifo_out：从FIFO缓冲区中取出多个元素。
kfifo_len：返回FIFO缓冲区中的元素个数。
kfifo_is_empty：判断FIFO缓冲区是否为空。
kfifo_is_full：判断FIFO缓冲区是否已满。
kfifo_reset：重置FIFO缓冲区，清空其中的所有元素。
kfifo_skip：跳过FIFO缓冲区中的一个元素。
kfifo_peek：查看FIFO缓冲区中的第一个元素，但不取出。


kfifo_initialized：检查一个kfifo是否已经初始化。
kfifo_esize：返回一个kfifo中每个元素的大小（字节）。
kfifo_recsize：返回一个kfifo中记录长度的大小（字节）。
kfifo_size：返回一个kfifo的总容量（字节）。
kfifo_reset：重置一个kfifo，清空其中的数据。
kfifo_reset_out：重置一个kfifo的输出指针，使其指向缓冲区的开头。
kfifo_len：返回一个kfifo中已经存储的数据的长度（字节）。
kfifo_is_empty：检查一个kfifo是否为空。
kfifo_is_full：检查一个kfifo是否已满。
kfifo_avail：返回一个kfifo中还可以存储的数据的长度（字节）。
kfifo_skip：跳过一个kfifo中的一定数量的数据（字节）。
kfifo_peek_len：返回一个kfifo中第一个记录的长度（字节）。
kfifo_alloc：动态分配一个kfifo，并初始化它。
kfifo_free：释放一个kfifo的内存。
kfifo_init：用一个已经分配的缓冲区初始化一个kfifo。
kfifo_put：向一个kfifo中存入一个元素。
kfifo_get：从一个kfifo中取出一个元素。
kfifo_peek：查看一个kfifo中的第一个元素，但不取出。
kfifo_in：向一个kfifo中存入一段数据（字节）。
kfifo_in_spinlocked：在一个自旋锁的保护下，向一个kfifo中存入一段数据（字节）。
kfifo_in_locked：在一个互斥锁的保护下，向一个kfifo中存入一段数据（字节）。
kfifo_out：从一个kfifo中取出一段数据（字节）。
kfifo_out_spinlocked：在一个自旋锁的保护下，从一个kfifo中取出一段数据（字节）。
kfifo_out_locked：在一个互斥锁的保护下，从一个kfifo中取出一段数据（字节）。
kfifo_from_user：从用户空间复制一段数据（字节），并存入一个kfifo中。
kfifo_to_user：从一个kfifo中取出一段数据（字节），并复制到用户空间。
kfifo_dma_in_prepare：准备一个kfifo的DMA输入操作。
kfifo_dma_in_finish：完成一个kfifo的DMA输入操作。
kfifo_dma_out_prepare：准备一个kfifo的DMA输出操作。
kfifo_dma_out_finish：完成一个kfifo的DMA输出操作。
kfifo_out_peek：从一个kfifo中复制一段数据（字节），但不取出。