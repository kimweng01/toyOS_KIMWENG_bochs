#include "sync.h"
#include "list.h"
#include "global.h"
#include "debug.h"
#include "interrupt.h"

/* 初始化信号量 */
void sema_init(struct semaphore* psema, uint8_t value) {
   psema->value = value;       // 为信号量赋初值
   list_init(&psema->waiters); //初始化信号量的等待队列
}


/* 初始化锁plock */
void lock_init(struct lock* plock) {
   plock->holder = NULL;
   plock->holder_repeat_nr = 0;
   sema_init(&plock->semaphore, 1);  // 信号量初值为1
}


/* 信号量down操作 */
void sema_down(struct semaphore* psema) {
/* 关中断来保证原子操作 */
   enum intr_status old_status = intr_disable();
   while(psema->value == 0) {	// 若value为0,表示已经被别人持有
      ASSERT(!elem_find(&psema->waiters, &running_thread()->general_tag));
      /* 当前线程不应该已在信号量的waiters队列中 */
      if (elem_find(&psema->waiters, &running_thread()->general_tag)) {
		PANIC("sema_down: thread blocked has been in waiters_list\n");
      }
/* 若信号量的值等于0,则当前线程把自己加入该锁的等待队列,然后阻塞自己 */
      list_append(&psema->waiters, &running_thread()->general_tag); 
      thread_block(TASK_BLOCKED);    // 阻塞线程,直到被唤醒
   }
/* 若value为1或被唤醒后,会执行下面的代码,也就是获得了锁。*/
   psema->value--;
   ASSERT(psema->value == 0);	    
/* 恢复之前的中断状态 */
   intr_set_status(old_status);
}
/*	###需要注意:
	###sema_down函數內的while(psema->value == 0)前面需要關中斷，
	###假如不關中斷，則while(psema->value == 0)判斷結束的一剎那時脈中斷來臨，A還來不及psema->valu--就被換給B，
	###如果B進入PV時，成功執行lock_acquire內的psema->valu--，使psema->valu為0，
	###然後在執行PV內的程式時時間到了，如果此時B要把棒交回給A，會回到A當初被中斷的地方，即while(psema->value == 0)之後、psema->valu--之前，
	###此時A已經在判斷while(psema->value == 0)後面的地方了，所以接著會執行psema->valu--，把psema->valu變成-1，
	###這樣就會出錯，所以sema_down函數內的while(psema->value == 0)前面需要關中斷!
	###
	###離開sema_down函數時要 "恢復關中斷前的設置" ，
	###因為A可能再執行一個需要關中斷的程式，而程式內又有一部分需要PV，
	###這樣離開sema_down函數後才能回到進入sema_down函數前的設置。
	###
	###補充說明:
	###而在main交棒給還沒執行過的A時，會經過thread.c的kernel_thread函數，									<==========重要!!!
	###此時是直接intr_enable()，
	###因為A還沒執行過，所以交給A的時候要給它最初始的狀態，即交棒給A時直接開中斷!
	###
	###如果某執行緒(例如main)執行過了，棒交回給main時，根據前面分析，執行過的程式會從kernel.S的iretd返回，
	###由於會進入中斷處理常式一定是在開中斷的情況下，所以iretd指令會讓CPU自動開中斷!
	###也就是回到進入中斷處理常式前的狀態。	*/


/* 信号量的up操作 */
void sema_up(struct semaphore* psema) {
/* 关中断,保证原子操作 */
   enum intr_status old_status = intr_disable();
   ASSERT(psema->value == 0);	    
   if (!list_empty(&psema->waiters)) {
      struct task_struct* thread_blocked = elem2entry(struct task_struct, general_tag, list_pop(&psema->waiters));
      thread_unblock(thread_blocked);
   }
   psema->value++;
   ASSERT(psema->value == 1);	    
/* 恢复之前的中断状态 */
   intr_set_status(old_status);
}
/*	###需要注意:
	###此elem2entry內含有list_pop(&psema->waiters)，
	###等於會執行list_pop函數，把等待隊列(waiters)的第一個執行緒pop掉!
	###
	###sema_up函數內有改變鏈結串列的函數，避免沒接完A的時間就到，然後下一棒要用沒接完的鏈結串列的情況，
	###所以sema_up函數內要關中斷，離開後要回到進入 sema_up函數前 的狀態
	###(傳入intr_set_status函數的old_status是定義在 sema_up函數內的開頭 的)。	*/


/* 获取锁plock */
void lock_acquire(struct lock* plock) {
/* 排除曾经自己已经持有锁但还未将其释放的情况。*/
   if (plock->holder != running_thread()) { 
      sema_down(&plock->semaphore);    // 对信号量P操作,原子操作
      plock->holder = running_thread();
      ASSERT(plock->holder_repeat_nr == 0);
      plock->holder_repeat_nr = 1;
   } 
   else {
      plock->holder_repeat_nr++;
   }
}
/*	###需要注意:
	###因為A可能有巢狀的PV，即:
	###
	###			P()
	###				P()
	###				
	###				V()
	###			P()
	###
	###每摸到一次P就要申請一次鎖，所以A可能有申請兩次鎖的情況，
	###如果A摸到第二個P而要申請第二的鎖，
	###即lock_acquire內的if(plocl->holder != running_thread不成立)，
	###plock->holder_repeat_nr直接+1即可，
	###之後在V操作時每摸到一次V會減一次1回去。	*/


/* 释放锁plock */
void lock_release(struct lock* plock) {
   ASSERT(plock->holder == running_thread());
   if (plock->holder_repeat_nr > 1) {
      plock->holder_repeat_nr--;
      return;
   }
   ASSERT(plock->holder_repeat_nr == 1);

   plock->holder = NULL;	   // 把锁的持有者置空放在V操作之前
   plock->holder_repeat_nr = 0;
   sema_up(&plock->semaphore);	   // 信号量的V操作,也是原子操作
}
/*	###需要注意:
	###lock_release函數的plock->holder = NULL的操作必須放在sema_up之前，
	###因為lock_release函數運行時不會關閉中斷，如果sema_up在plock->holder = NULL前面，
	###sema_up會把psema->value變回1，變成1表示開始會有人搶鎖，如果這個時候還來不及plock->holder = NULL結果A的時間到，
	###A交棒給B，此時若B如果進入PV(psema->value已經等於1了，所以B可以進PV區)，會把鎖拿走並把plock->holder = B，
	###此時假如B在PV內時間到了，把棒交回給A時，由於A執行過，根據之前推論會回到A被中斷的地方，
	###也就是sema_up函數 與 plock->holder = NULL 的中間，
	###此時A會執行下一條程式，即plock->holder = NULL，把plock->holder = B 變成 plock->holder = NULL，造成混亂，
	###所以lock_release函數的plock->holder = NULL的操作必須放在sema_up之前!	*/
