#include "ioqueue.h"
#include "interrupt.h"
#include "global.h"
#include "debug.h"

/* 初始化io队列ioq */
void ioqueue_init(struct ioqueue* ioq) {
   lock_init(&ioq->lock);     // 初始化io队列的锁
   ioq->producer = ioq->consumer = NULL;  // 生产者和消费者置空
   ioq->head = ioq->tail = 0; // 队列的首尾指针指向缓冲区数组第0个位置
}

/* 返回pos在缓冲区中的下一个位置值 */
static int32_t next_pos(int32_t pos) {
   return (pos + 1) % bufsize; 
}

/* 判断队列是否已满 */
bool ioq_full(struct ioqueue* ioq) {
   ASSERT(intr_get_status() == INTR_OFF);
   return next_pos(ioq->head) == ioq->tail;
}
/*	###head是生產者，tail是消費者
	###當生產者跑到tail的前一格時，代表差一格就要滿了，
	###然後這時就要準備block生產者，
	###會故意差一格的原因是如果head繞了一圈跟tail重合了，
	###會沒辦法判斷現在緩衝區是空還是滿，
	###所以故意讓生產者還差一格就要被block，
	###這樣如果緩衝區有64格，能用的就只有63格。	*/

/* 判断队列是否已空 */
bool ioq_empty(struct ioqueue* ioq) {
   ASSERT(intr_get_status() == INTR_OFF);
   return ioq->head == ioq->tail;
}


//=================================================================
/* 使当前生产者或消费者在此缓冲区上等待 */
static void ioq_wait(struct task_struct** waiter) {
   ASSERT(*waiter == NULL && waiter != NULL);
   *waiter = running_thread();
   thread_block(TASK_BLOCKED);
}

/* 唤醒waiter */
static void wakeup(struct task_struct** waiter) {
   ASSERT(*waiter != NULL);
   thread_unblock(*waiter); 
   *waiter = NULL;
}

//-----------------------------------------------
/* 消费者从ioq队列中获取一个字符 */
char ioq_getchar(struct ioqueue* ioq) {
   ASSERT(intr_get_status() == INTR_OFF);

/* 若缓冲区(队列)为空,把消费者ioq->consumer记为当前线程自己,
 * 目的是将来生产者往缓冲区里装商品后,生产者知道唤醒哪个消费者,
 * 也就是唤醒当前线程自己*/
   while (ioq_empty(ioq)) {
      lock_acquire(&ioq->lock);	 
      ioq_wait(&ioq->consumer);
      lock_release(&ioq->lock);
   }
/*	###也可以寫成:
	###	while (ioq_empty(ioq)) {
	###    lock_acquire(&ioq->lock);	
	###  
	###    ASSERT(ioq->consumer == NULL);
	###    ioq->consumer = running_thread();
	###    thread_block(TASK_BLOCKED);
	###
	###    lock_release(&ioq->lock);
	### }
	### 
	### 或
	### 
	### static void ioq_wait(struct ioqueue* ioq) {
	### ASSERT(ioq->consumer == NULL);
	### ioq->consumer = running_thread();
	### thread_block(TASK_BLOCKED);
	### }
	### 
	### while (ioq_empty(ioq)) {
	###    lock_acquire(&ioq->lock);	 
	###    ioq_wait(ioq);
	###    lock_release(&ioq->lock);
	### }
	### 
	### 只要能確實改變struct ioqueue內的struct task_struct* consumer的值皆可，
	### ioq_putchar的while迴圈亦同。 */

   char byte = ioq->buf[ioq->tail];	  // 从缓冲区中取出
   ioq->tail = next_pos(ioq->tail);	  // 把读游标移到下一位置

   if (ioq->producer != NULL) {
      wakeup(&ioq->producer);		  // 唤醒生产者
   }

   return byte; 
}

/* 生产者往ioq队列中写入一个字符byte */
void ioq_putchar(struct ioqueue* ioq, char byte) {
   ASSERT(intr_get_status() == INTR_OFF);

/* 若缓冲区(队列)已经满了,把生产者ioq->producer记为自己,
 * 为的是当缓冲区里的东西被消费者取完后让消费者知道唤醒哪个生产者,
 * 也就是唤醒当前线程自己*/
   while (ioq_full(ioq)) {
      lock_acquire(&ioq->lock);
      ioq_wait(&ioq->producer);
      lock_release(&ioq->lock);
   }
   
   ioq->buf[ioq->head] = byte;      // 把字节放入缓冲区中
   ioq->head = next_pos(ioq->head); // 把写游标移到下一位置

   if (ioq->consumer != NULL) {
      wakeup(&ioq->consumer);          // 唤醒消费者
   }
}

