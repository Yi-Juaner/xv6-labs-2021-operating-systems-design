// Buffer cache.
//
// The hash buckets make hits on different blocks proceed in parallel.
// evict_lock serializes cache misses so that at most one cached copy of a
// (dev, blockno) pair can be created.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
#define HASH(blockno) ((blockno) % NBUCKET)

struct bucket {
  struct spinlock lock;
  struct buf head;
};

struct {
  struct spinlock evict_lock;
  struct buf buf[NBUF];
  struct bucket buckets[NBUCKET];
} bcache;

// Caller must hold the bucket lock containing b.
static void
buf_remove(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

// Caller must hold dst->lock.
static void
buf_insert(struct bucket *dst, struct buf *b)
{
  b->next = dst->head.next;
  b->prev = &dst->head;
  dst->head.next->prev = b;
  dst->head.next = b;
}

void
binit(void)
{
  initlock(&bcache.evict_lock, "bcache.evict");

  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.buckets[i].lock, "bcache.bucket");
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
  }

  // 将未使用的缓冲区平均放入各个桶。
  for(int i = 0; i < NBUF; i++){
    struct buf *b = &bcache.buf[i];
    struct bucket *bucket = &bcache.buckets[i % NBUCKET];

    // 使用不可能的设备号，避免初始化缓冲区被误认为有效缓存。
    b->dev = (uint)-1;
    b->blockno = 0;
    b->valid = 0;
    b->refcnt = 0;

    initsleeplock(&b->lock, "buffer");
    buf_insert(bucket, b);
  }
}

// Look through buffer cache for block (dev, blockno).
// If not found, reuse an unreferenced buffer.
// Return the buffer with its sleep lock held.
static struct buf*
bget(uint dev, uint blockno)
{
  int target_id = HASH(blockno);
  struct bucket *target = &bcache.buckets[target_id];
  struct buf *b;

  // 快速路径：缓存命中时只获取目标桶锁。
  acquire(&target->lock);

  for(b = target->head.next;
      b != &target->head;
      b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&target->lock);

      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&target->lock);

  /*
   * 缓存未命中。
   *
   * 使用 evict_lock 串行化查找和缓冲区回收，
   * 保证同一个磁盘块最多只有一个缓存副本。
   */
  acquire(&bcache.evict_lock);

  /*
   * 获取 evict_lock 之前，其他 CPU 可能已经加载了目标块，
   * 因此需要再次检查。
   */
  acquire(&target->lock);

  for(b = target->head.next;
      b != &target->head;
      b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      release(&target->lock);
      release(&bcache.evict_lock);

      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&target->lock);

  /*
   * 在所有桶中寻找 refcnt == 0 的缓冲区。
   *
   * 检查 refcnt 后不能立即释放原桶锁，否则另一个 CPU
   * 可能在缓冲区迁移前命中旧块并增加 refcnt。
   */
  for(int source_id = 0;
      source_id < NBUCKET;
      source_id++){
    struct bucket *source = &bcache.buckets[source_id];

    acquire(&source->lock);

    for(b = source->head.next;
        b != &source->head;
        b = b->next){
      if(b->refcnt != 0)
        continue;

      if(source_id == target_id){
        /*
         * 原桶和目标桶相同，不需要移动链表节点。
         * 在持有桶锁时直接更新缓冲区身份。
         */
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        release(&source->lock);
        release(&bcache.evict_lock);

        acquiresleep(&b->lock);
        return b;
      }

      /*
       * evict_lock 保证只有当前 CPU 会执行跨桶迁移。
       * 普通命中、brelse、bpin、bunpin 都只获取一把桶锁，
       * 因此不会形成两桶锁的循环等待。
       */
      acquire(&target->lock);

      buf_remove(b);
      buf_insert(target, b);

      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      release(&target->lock);
      release(&source->lock);
      release(&bcache.evict_lock);

      acquiresleep(&b->lock);
      return b;
    }

    release(&source->lock);
  }

  release(&bcache.evict_lock);
  panic("bget: no free buffers");
}

// Return a locked buf containing the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);

  if(!b->valid){
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }

  return b;
}

// Write b's contents to disk.
// The caller must hold b's sleep lock.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");

  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
void
brelse(struct buf *b)
{
  int id;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  /*
   * 此时 refcnt 仍大于 0，所以回收线程不能修改 blockno，
   * 根据 blockno 得到的桶编号是稳定的。
   */
  id = HASH(b->blockno);

  acquire(&bcache.buckets[id].lock);
  b->refcnt--;
  release(&bcache.buckets[id].lock);
}

void
bpin(struct buf *b)
{
  int id = HASH(b->blockno);

  acquire(&bcache.buckets[id].lock);
  b->refcnt++;
  release(&bcache.buckets[id].lock);
}

void
bunpin(struct buf *b)
{
  int id = HASH(b->blockno);

  acquire(&bcache.buckets[id].lock);
  b->refcnt--;
  release(&bcache.buckets[id].lock);
}

