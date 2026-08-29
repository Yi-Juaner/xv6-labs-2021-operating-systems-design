// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13          // 桶的数量（质数）
#define HASH(blockno) ((blockno) % NBUCKET)

struct bucket {
  struct spinlock lock;
  struct buf head;          // 链表头（双向）
};

struct {
  struct spinlock evict_lock;   // 序列化 eviction
  struct buf buf[NBUF];         // 所有缓冲区数组
  struct bucket buckets[NBUCKET];
} bcache;

void
binit(void)
{
  // 初始化每个桶
  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.buckets[i].lock, "bcache.bucket");
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
  }
  initlock(&bcache.evict_lock, "bcache.evict");

  // 将所有缓冲区放入桶 0
  struct bucket *b0 = &bcache.buckets[0];
  for (int i = 0; i < NBUF; i++) {
    struct buf *bp = &bcache.buf[i];
    bp->dev = 0;
    bp->blockno = 0;
    bp->valid = 0;
    bp->refcnt = 0;
    bp->timestamp = 0;
    bp->mybucket = b0;    // 记录所在桶
    initsleeplock(&bp->lock, "buffer");
    // 插入到桶 0 的链表头部
    bp->next = b0->head.next;
    bp->prev = &b0->head;
    b0->head.next->prev = bp;
    b0->head.next = bp;
  }
}


// 从当前桶的链表中移除 bp（需持有桶锁）
static void
buf_remove(struct buf *bp)
{
  bp->prev->next = bp->next;
  bp->next->prev = bp->prev;
}

// 将 bp 插入到指定桶的链表头部（需持有桶锁）
static void
buf_insert(struct buf *bp, struct bucket *b)
{
  bp->next = b->head.next;
  bp->prev = &b->head;
  b->head.next->prev = bp;
  b->head.next = bp;
  bp->mybucket = b;   // 更新所属桶
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  int bid = HASH(blockno);
  struct bucket *b = &bcache.buckets[bid];
  struct buf *bp;

  // 第一次查找
  acquire(&b->lock);
  for (bp = b->head.next; bp != &b->head; bp = bp->next) {
    if (bp->dev == dev && bp->blockno == blockno) {
      bp->refcnt++;
      release(&b->lock);
      acquiresleep(&bp->lock);
      return bp;
    }
  }
  release(&b->lock);

  // 未找到，需要分配新块，序列化 eviction
  acquire(&bcache.evict_lock);

  // double-check: 可能其他 CPU 已插入
  acquire(&b->lock);
  for (bp = b->head.next; bp != &b->head; bp = bp->next) {
    if (bp->dev == dev && bp->blockno == blockno) {
      bp->refcnt++;
      release(&b->lock);
      release(&bcache.evict_lock);
      acquiresleep(&bp->lock);
      return bp;
    }
  }
  release(&b->lock);

  // 选择 LRU 缓冲区（refcnt==0 且 timestamp 最小）
  bp = 0;
  int oldest =0x7fffffff;
  int old_bid = -1;

  // 遍历所有桶，查找可用的缓冲区
  for (int i = 0; i < NBUCKET; i++) {
    struct bucket *bptr = &bcache.buckets[i];
    acquire(&bptr->lock);
    for (struct buf *c = bptr->head.next; c != &bptr->head; c = c->next) {
      if (c->refcnt == 0 && c->timestamp <= oldest) {
        oldest = c->timestamp;
        bp = c;
        old_bid = i;
      }
    }
    release(&bptr->lock);
  }
  if (bp == 0)
    panic("bget: no free buffers");

  // 从原桶移除，加入目标桶
  struct bucket *old_bucket = &bcache.buckets[old_bid];
  struct bucket *new_bucket = &bcache.buckets[bid];

  // 按桶索引升序获取锁，避免死锁
  if (old_bid < bid) {
    acquire(&old_bucket->lock);
    acquire(&new_bucket->lock);
  } else if (old_bid > bid) {
    acquire(&new_bucket->lock);
    acquire(&old_bucket->lock);
  } else {
    // 同一个桶
    acquire(&old_bucket->lock);
  }

  // 从旧桶移除
  buf_remove(bp);
  // 插入新桶
  buf_insert(bp, new_bucket);

  // 设置新块信息
  bp->dev = dev;
  bp->blockno = blockno;
  bp->valid = 0;
  bp->refcnt = 1;
  bp->timestamp = ticks;

  // 解锁（按相反顺序）
  if (old_bid < bid) {
    release(&new_bucket->lock);
    release(&old_bucket->lock);
  } else if (old_bid > bid) {
    release(&old_bucket->lock);
    release(&new_bucket->lock);
  } else {
    release(&old_bucket->lock);
  }
  release(&bcache.evict_lock);

  acquiresleep(&bp->lock);
  return bp;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // 使用 b->mybucket 快速定位所属桶
  acquire(&b->mybucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->timestamp = ticks;   // 更新最后使用时间
  }
  release(&b->mybucket->lock);
}

void
bpin(struct buf *b) {
  acquire(&b->mybucket->lock);
  b->refcnt++;
  release(&b->mybucket->lock);
}

void
bunpin(struct buf *b) {
  acquire(&b->mybucket->lock);
  b->refcnt--;
  release(&b->mybucket->lock);
}


