#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"

//~~~~~~~~~~~~第10章b~~~~~~~~~~~~~~
#include "debug.h"
#include "interrupt.h"
#include "print.h"

//~~~~~~~~~~~~第11章b~~~~~~~~~~~~~~
#include "process.h"

//~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~
#include "sync.h"

//~~~~~~~~~~~~第15章e~~~~~~~~~~~~~~
#include "stdio.h"
#include "file.h"
#include "fs.h"


//#define PG_SIZE 4096 已經定義在global.h中


//~~~~~~~~~~~~~~~~~~~~~~~第10章b~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
struct task_struct* main_thread;    // 主线程PCB

//~~~~~~~~~~~~~~~~~~~~~~~第13章~~~~~~~~~~~~~~~~~~~~~~~~~~
struct task_struct* idle_thread;    // idle线程

struct list thread_ready_list;	    // 就绪队列
struct list thread_all_list;	    // 所有任务队列

//~~~~~~~~~~~~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~~~~~~~~~~~~
struct lock pid_lock;		    	// 分配pid锁
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static struct list_elem* thread_tag;// 用于保存队列中的线程结点

extern void switch_to(struct task_struct* cur, struct task_struct* next);

//~~~~~~~~~~~~~~~~~~~~~~~第15章a~~~~~~~~~~~~~~~~~~~~~~~~~
extern void init(void);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//~~~~~~~~~~~~~~~~~~~~~~~第13章~~~~~~~~~~~~~~~~~~~~~~~~~
/* 系统空闲时运行的线程 */
static void idle(void* arg UNUSED) {
   while(1) {
      thread_block(TASK_BLOCKED);  
	  
      //执行hlt时必须要保证目前处在开中断的情况下
      asm volatile ("sti; hlt" : : : "memory");
	  //###一定要用sti先開中斷，否則hlt後所有的外部中斷都不會有影響了。
   }
}
/*
###需要注意:
###	在thread_init函數內封裝了idle之執行緒，該執行緒會被放在就緒對列中，
###	當交棒到idle的時候，會直接進入thread_block函數把自己"直接"交棒出去，不把自己放入任何一個等待隊列，
###	等到在schedule函數內發現就緒隊列為空時，就會立刻用thread_unblock函數把idle放到就緒隊列，
###	然後交棒就會給到idle，然後idle接著就會執行後面的code，也就是asm volatile ("sti; hlt" : : : "memory")，
###	先開中斷再阻塞自己，如果不先開中斷，hlt後所有的外部中斷都不會有影響了，
###	自己CPU完全不接受任何指令，直到有一個外部中斷進來，
###	當外部中斷發生後，會先進入中斷處理常式，離開中斷處理常式後程式會回到進入中斷處理常式前的地方，也就是hlt指令的位置，繼續直行接下的code，
###	然後再迴圈一次，進入thread_block函數直接把自己交棒出去，不加入任何等待隊列...如此呈現一個循環。
*/

//~~~~~~~~~~~~~~~~~~~~~~~第10章b~~~~~~~~~~~~~~~~~~~~~~~~~
/* 获取当前线程pcb指针 */
struct task_struct* running_thread() {
   uint32_t esp; 
   asm ("mov %%esp, %0" : "=g" (esp));
  /* 取esp整数部分即pcb起始地址 */
   return (struct task_struct*)(esp & 0xfffff000);
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


/* 由kernel_thread去执行function(func_arg) */
static void kernel_thread(thread_func* function, void* func_arg) {
	intr_enable();	//##<---此內容於第10章b被加入
	/*	###因為之後都是用中斷來切換執行緒，若下一棒是第一次執行，要開中斷，
		####即交給沒執行過的下一棒要給它最初始的狀態	*/

    function(func_arg); 
}


//~~~~~~~~~~~~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~~~~~~~~~~~~
/* 分配pid */
static pid_t allocate_pid(void) {
   static pid_t next_pid = 0;
   lock_acquire(&pid_lock);
   
   next_pid++;
/*	###next_pid++可大致拆解為:
	###	move, [num] 1
	###	move, eax [next_pid]
	###	add,  eax [num]
	###	move  [next_pid] eax
	###	
	###	因為next_pid的型別是"static"!!!
	###	假如現在next_pid是3
	###	若在add,  eax [num]的途中因為時間到突然被換掉，
	###	下一棒把next_pid加為4，
	###	棒換回來後，因為add,  eax [num]的結果還儲存在eax中，
	###	所以會把4給next_pid，
	###	這樣有兩個4，就重複了，
	###	所以next_pid++要用PV包起來。	*/
   
   lock_release(&pid_lock);
   return next_pid;
}


//~~~~~~~~~~~~~~~~~~~~~~~第15章a~~~~~~~~~~~~~~~~~~~~~~~~~
/* fork进程时为其分配pid,因为allocate_pid已经是静态的,别的文件无法调用.
不想改变函数定义了,故定义fork_pid函数来封装一下。*/
pid_t fork_pid(void) {
   return allocate_pid();
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


/* 初始化线程栈thread_stack,将待执行的函数和参数放到thread_stack中相应的位置 */
void thread_create(struct task_struct* pthread, thread_func function, void* func_arg) {
    /* 先预留中断使用栈的空间,可见thread.h中定义的结构 */
    pthread->self_kstack -= sizeof(struct intr_stack);
	/* 再留出线程栈空间,可见thread.h中定义 */
    pthread->self_kstack -= sizeof(struct thread_stack);
	
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->function = function;
    kthread_stack->func_arg = func_arg;
    kthread_stack->ebp = kthread_stack->ebx = kthread_stack->esi = kthread_stack->edi = 0;
}


/* 初始化线程基本信息 */
void init_thread(struct task_struct* pthread, char* name, int prio) {
    memset(pthread, 0, sizeof(*pthread));
	
//~~~~~~~~~~~~~~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~~~~~~~~~~~~~~
	pthread->pid = allocate_pid();
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	
    strcpy(pthread->name, name);
	
//~~~~~~~~~~~~~~~~~~~~~~~~~~~第9章c~~~~~~~~~~~~~~~~~~~~~~~~~~~
	if (pthread == main_thread) {
	/* 由于把main函数也封装成一个线程,并且它一直是运行的,故将其直接设为TASK_RUNNING */
      pthread->status = TASK_RUNNING;
	} 
	else {
      pthread->status = TASK_READY;
	}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	//pthread->status = TASK_RUNNING; <---##第9章c以前設成此
	pthread->priority = prio;
    /* ###self_kstack是线程自己在内核态下使用的栈顶地址 */
    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);
	
//~~~~~~~~~~~~~~~~~~~~~~~~~~~第9章c~~~~~~~~~~~~~~~~~~~~~~~~~~~
	pthread->ticks = prio;
	pthread->elapsed_ticks = 0;
	pthread->pgdir = NULL; //###執行緒沒有自己的位址空間，這是給使用者用的，設為NULL!

//~~~~~~~~~~~~~~~~~~~~~~~~~~~第14章c~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   /* 预留标准输入输出 */
   pthread->fd_table[0] = 0;
   pthread->fd_table[1] = 1;
   pthread->fd_table[2] = 2;
   /* 其余的全置为-1 */
   uint8_t fd_idx = 3;
   while (fd_idx < MAX_FILES_OPEN_PER_PROC) {
      pthread->fd_table[fd_idx] = -1;
      fd_idx++;
   }

//~~~~~~~~~~~~~~~~~~~~~~~~~~~第14章l~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   pthread->cwd_inode_nr = 0;	    // 以根目录做为默认工作路径

//~~~~~~~~~~~~~~~~~~~~~~~~~~~第15章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   pthread->parent_pid = -1;        // -1表示没有父进程
   
   pthread->stack_magic = 0x19960927;	  // 自定义的魔数
}


/* 创建一优先级为prio的线程,线程名为name,线程所执行的函数是function(func_arg) */
/*
*  name:线程名 
*  prio:线程优先级
*  function:执行函数
*  func_arg:函数参数
* */
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg) {
	/*												   ^~~~~~~~~~~~~~~~~~~~
														等於void function(void*) 
														
														舉例: typedef signed int int32_t
															  int32_t num; ==> signed int num;
															  
														所以: typedef void 名字(void*)  thread_func
															  thread_func function ==> void 名字(void*) function ==> void function(void*) 
	*/
	
    /* pcb都位于内核空间,包括用户进程的pcb也是在内核空间 */
    struct task_struct* thread = get_kernel_pages(1);
	
    init_thread(thread, name, prio);
    thread_create(thread, function, func_arg);
	
	/* 确保之前不在队列中 */
	ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
	/* 加入就绪线程队列 */
	list_append(&thread_ready_list, &thread->general_tag);
	
	/* 确保之前不在队列中 */
	ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
	/* 加入全部线程队列 */
	list_append(&thread_all_list, &thread->all_list_tag);
	
//~~~~~~~~~~~~~~~~~~~~~~~~~~~第9章a~~~~~~~~~~~~~~~~~~~~~~~~~~~
	/*
	###P.S. 現在thread_start函數內沒有這條code:
	asm volatile ("movl %0, %%esp; pop %%ebp; pop %%ebx; pop %%edi; pop %%esi; ret" : : "g" (thread->self_kstack) : "memory")，
	###這是在 第9章a 的時候測試PCB用的，當時不需時脈中斷，thread_start函數內最後執行此code即可執行被封裝的函數!
	*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	
    return thread;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~第9章c~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 将kernel中的main函数完善为主线程 */
static void make_main_thread(void) {
/* 因为main线程早已运行,咱们在loader.S中进入内核时的mov esp,0xc009f000,
就是为其预留了tcb,地址为0xc009e000,因此不需要通过get_kernel_page另分配一页*/
   main_thread = running_thread();
   init_thread(main_thread, "main", 31);

/* main函数是当前线程,当前线程不在thread_ready_list中,
 * 所以只将其加在thread_all_list中. */
   ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
   list_append(&thread_all_list, &main_thread->all_list_tag);
}


/* 实现任务调度 */
void schedule() {

   ASSERT(intr_get_status() == INTR_OFF);

   struct task_struct* cur = running_thread(); 
   if (cur->status == TASK_RUNNING) { // 若此线程只是cpu时间片到了,将其加入到就绪队列尾
      ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
      list_append(&thread_ready_list, &cur->general_tag);
      cur->ticks = cur->priority;     // 重新将当前线程的ticks再重置为其priority;
      cur->status = TASK_READY;
   }
   /*
   else { 
      若此线程需要某事件发生后才能继续上cpu运行,
      不需要将其加入队列,因为当前线程不在就绪队列中。
   }
   ###如果不是因為時間到被送進schedule函數，比如說要進入別人正在使用的PV區被block，
   ###那目前執行緒已經被lock_acquire函數加入等待隊列(struct list waiter)，
   ###不能再被放入就緒隊列了，所以程式直接執行schedule函數的下面。
   */
   
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第13章~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   /* 如果就绪队列中没有可运行的任务,就唤醒idle */
   if (list_empty(&thread_ready_list)) {
      thread_unblock(idle_thread);
   }
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

   ASSERT(!list_empty(&thread_ready_list));
   thread_tag = NULL;	  // thread_tag清空
   //###因為thread_tag是全域變數，先賦予NULL比較保險
 
 
/* 将thread_ready_list队列中的第一个就绪线程弹出,准备将其调度上cpu. */
   thread_tag = list_pop(&thread_ready_list);   
   struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);
   next->status = TASK_RUNNING;

//~~~~~~~~~~~~~~~~~~~~第11章b~~~~~~~~~~~~~~~~~~
   process_activate(next);
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   
   switch_to(cur, next);
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~第10章b~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*
#############################################################################
##	锁结构:                                                          	   ##
##	struct lock {                                                          ##
##	   struct   task_struct* holder;	    // 锁的持有者                  ##
##	   struct   semaphore semaphore;	    // 用二元信号量实现锁          ##
##	   uint32_t holder_repeat_nr;		    // 锁的持有者重复申请锁的次数  ##
##	};                                                                     ##
#############################################################################

##########################################################################
##	信号结构:            												##
##	struct semaphore {         											##
##	   uint8_t  value;         											##
##	   struct   list waiters;  //紀錄在等鎖的持有者釋放的人(函數)有誰	##
##	};                         											##
##########################################################################

###需要注意:
###以上兩個結構同一種PV都只會有一個!!!
###同一種PV，鎖在同一個時間只會由一個執行緒佔有!
###
###以上的結構共有三層，struct   list waiters是鏈結串列，
###等待者會把自己加入鏈結串列waiters中。
###
###若A函數含有不想被打擾的螢幕輸出函數，除了用PV把這個函數包圍外，還可以在螢幕輸出函數的錢跟後分別關中斷跟開中斷，
###然而，假設螢幕輸出函數非常非常非常的長，且B、C都有螢幕輸出函數的函數，
###則在關中斷的情況下，B和C都要等A螢幕輸出函數執行完才能上CPU，
###而PV操作的好處是，當A在執行螢幕輸出函數時被換下處理器，
###如果輪到B，那B可以先執行被PV包圍的程式，等到執行到了PV處再被BLOCK，再輪到C，
###可以避免A的螢幕輸出函數的程式過長而讓A長期獨佔CPU!
*/


/* 当前线程将自己阻塞,标志其状态为stat. */
void thread_block(enum task_status stat) {
/* stat取值为TASK_BLOCKED,TASK_WAITING,TASK_HANGING,也就是只有这三种状态才不会被调度*/
   ASSERT(((stat == TASK_BLOCKED) || (stat == TASK_WAITING) || (stat == TASK_HANGING)));
   enum intr_status old_status = intr_disable();
   struct task_struct* cur_thread = running_thread();
   cur_thread->status = stat; 	// 置其状态为stat 
   
   schedule();		      		// 将当前线程换下处理器
   
/* 待当前线程被解除阻塞后才继续运行下面的intr_set_status */
   intr_set_status(old_status);
}


/* 将线程pthread解除阻塞 */
void thread_unblock(struct task_struct* pthread) {
   enum intr_status old_status = intr_disable();
   ASSERT(((pthread->status == TASK_BLOCKED) || (pthread->status == TASK_WAITING) || (pthread->status == TASK_HANGING)));
   if (pthread->status != TASK_READY) {
      ASSERT(!elem_find(&thread_ready_list, &pthread->general_tag));
      if (elem_find(&thread_ready_list, &pthread->general_tag)) {
		PANIC("thread_unblock: blocked thread in ready_list\n");
      }
      list_push(&thread_ready_list, &pthread->general_tag);    // 放到队列的最前面,使其尽快得到调度
      pthread->status = TASK_READY;
   } 
   intr_set_status(old_status);
}
/*	###需要注意:
	###thread_block 和 thread_unblock 都是要改變鏈結串列的函數，
	###如果鏈結串列還沒接完，結果時間到棒被交出去，下一棒就會用到沒接完的鏈結串列，
	###所以關係到鏈結串列的函數為了保險起見，函數前後還是用關中斷、恢復關中斷前的中斷狀態把函數包起來，
	###
	###thread_block 和 thread_unblock的最後不是直接開中斷，而是用intr_set_status(old_status)，
	###即恢復到 關中斷前的中斷狀態 ，因為進入到 thread_block 或 thread_unblock 前程式可能也是在關中斷的狀態，
	###所以離開 thread_block 或 thread_unblock 後，要回到 進入 thread_block 或 thread_unblock 前 的狀態，
	###
	###intr_set_status(old_status)的 old_status 是定義在 thread_block函數 和 thread_unblock函數內的，
	###即enum intr_status old_status，因此B要 恢復關中斷前的中斷狀態時，
	###傳進intr_set_status的 參數是 定義在thread_block函數開頭的old_status ，
	###也就是恢復到當初B進入到thread_block函數時 關中斷前的狀態。	*/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~第13章~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 主动让出cpu,换其它线程运行 */
void thread_yield(void) {
   struct task_struct* cur = running_thread();   
   enum intr_status old_status = intr_disable();
   ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
   list_append(&thread_ready_list, &cur->general_tag);
   cur->status = TASK_READY;
   schedule();
   intr_set_status(old_status);
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~第15章e~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 以填充空格的方式输出buf */
static void pad_print(char* buf, int32_t buf_len, void* ptr, char format) {
   memset(buf, 0, buf_len);
   uint8_t out_pad_0idx = 0;
   switch(format) {
      case 's':
		out_pad_0idx = sprintf(buf, "%s", ptr);
		break;
      case 'd':
		out_pad_0idx = sprintf(buf, "%d", *((int16_t*)ptr));
		//#這裡不加break? 那結果會被下一個case覆蓋掉，所以特別設'd'處理16位元的pid號等於白做工?
      case 'x':
		out_pad_0idx = sprintf(buf, "%x", *((uint32_t*)ptr));
   }
   while(out_pad_0idx < buf_len) { // 以空格填充
      buf[out_pad_0idx] = ' ';
      out_pad_0idx++;
   }
   sys_write(stdout_no, buf, buf_len - 1);
}

/* 用于在list_traversal函数中的回调函数,用于针对线程队列的处理 */
static bool elem2thread_info(struct list_elem* pelem, int arg UNUSED) {
   struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
   char out_pad[16] = {0};

   pad_print(out_pad, 16, &pthread->pid, 'd');

   if (pthread->parent_pid == -1) {
      pad_print(out_pad, 16, "NULL", 's');
   } else { 
      pad_print(out_pad, 16, &pthread->parent_pid, 'd');
   }

   switch (pthread->status) {
      case 0:
		pad_print(out_pad, 16, "RUNNING", 's');
		break;
      case 1:
		pad_print(out_pad, 16, "READY", 's');
		break;
      case 2:
		pad_print(out_pad, 16, "BLOCKED", 's');
		break;
      case 3:
		pad_print(out_pad, 16, "WAITING", 's');
		break;
      case 4:
		pad_print(out_pad, 16, "HANGING", 's');
		break;
      case 5:
	 pad_print(out_pad, 16, "DIED", 's');
   }
   pad_print(out_pad, 16, &pthread->elapsed_ticks, 'x');

   memset(out_pad, 0, 16);
   ASSERT(strlen(pthread->name) < 17);
   memcpy(out_pad, pthread->name, strlen(pthread->name));
   strcat(out_pad, "\n");
   sys_write(stdout_no, out_pad, strlen(out_pad));
   return false;	// 此处返回false是为了迎合主调函数list_traversal,只有回调函数返回false时才会继续调用此函数
}

/* 打印任务列表 */
void sys_ps(void) {
   char* ps_title = "PID            PPID           STAT           TICKS          COMMAND\n";
   sys_write(stdout_no, ps_title, strlen(ps_title));
   list_traversal(&thread_all_list, elem2thread_info, 0);
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


/* 初始化线程环境 */
void thread_init(void) {
   put_str("thread_init start\n");
   list_init(&thread_ready_list);
   list_init(&thread_all_list);

//~~~~~~~~~~~~~~~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   lock_init(&pid_lock);

//~~~~~~~~~~~~~~~~~~~~~~~~~~第15章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ 
/* 先创建第一个用户进程:init */
   process_execute(init, "init");         // 放在第一个初始化,这是第一个进程,init进程的pid为1
   
//~~~~~~~~~~~~~~~~~~~~~~~~~~第9章c~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 将当前main函数创建为线程 */
   make_main_thread();
  
//~~~~~~~~~~~~~~~~~~~~~~~~~~第13章~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  
/* 创建idle线程 */
   idle_thread = thread_start("idle", 10, idle, NULL);
   
   put_str("thread_init done\n");
   
}


//#############################################################################################
/*
###thread_start封裝統整(從thread_start說起):
###
###	用get_kernel_pages函數申請一個分頁，獲得該分頁的基底虛擬位址，該分頁即為PCB，類型為void，
###	把該位址給thread，變成類型為struct task_struct*的位址。
###	
###	##################################################################################
###	##	struct task_struct {                                               			##
### ##		uint32_t* self_kstack;	// 各内核线程都用自己的内核栈                  	##
###	##		enum task_status status;                                                ##
###	##		char name[16];                                                          ##
###	##		uint8_t priority;                                                       ##
###	##		uint8_t ticks;	   		// 每次在处理器上执行的时间嘀嗒数               ##
### ##                                                                              ##
###	##		此任务自上cpu运行后至今占用了多少cpu嘀嗒数,                             ##
###	##		也就是此任务执行了多久                                                  ##
###	##		uint32_t elapsed_ticks;                                                 ##
### ##                                                                              ##
###	##		general_tag的作用是用于线程在一般的队列中的结点                         ##
###	##		struct list_elem general_tag;				                            ##
### ##                                                                              ##
###	##		all_list_tag的作用是用于线程队列thread_all_list中的结点                 ##
###	##		struct list_elem all_list_tag;                                          ##
###	##		                                                                        ##
###	##		uint32_t* pgdir;		// 进程自己页表的虚拟地址                		##
###	##		uint32_t stack_magic;	// 用这串数字做栈的边界标记,用于检测栈的溢出	##
###	##	};                                                                 			##
###	##################################################################################
###	
###	用init_thread傳入 分頁的基底虛擬位址(類型為struct task_struct*)、名字、			優先順序，
###		傳入後變為	  struct task_struct* pthread 				 	 char* name 	int prio，
###		
###		把 pthread(剛申請到的分頁的虛擬基底位址) 整個用 memset 清0，
###		把 名字(name) 複製到 pthread->name (strcpy要兩個參數都為NULL才會ASSERT，由於傳入的name不為NULL，所以不會ASSERT)，
###		pthread->status的 status 為 列舉task_status 內的 "標號"，把號碼TASK_RUNNING給status，
###		pthread->priority = prio即把優先順序(priority)賦予prio
###		pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE)即把 分頁頂的地址 給self_kstack，self_kstack為堆疊底，PG_SIZE為4096，
###		由於堆棧會由高地址往低地址生長，所以為了不撞到位在底層的struct task_struct，所以要定一個界線，目前暫定為0x19960927。
###
###	再用thread_create傳入 分頁的虛擬基底位址(thread)、	 
###	void function(void*)函數的 位址(function) 、
###	void function(void*)函數的 參數(func_arg) ，即void function(void*)是void function(void* func_arg)的原型。
###	需要注意，假如在main外宣告一個函數void func(int b[3])，若main裡面宣告int a[3]，則在main裡面可以用func(a)呼叫函數，
###	在fuuc內可以直接用a[X]的方式使用，函數宣告可以不用void func(int*)，此道理跟把functiont傳給thread_create相同。
###		傳入後參數分別變成:
###		struct task_struct* pthread、
###		thread_func function、
###		void* func_arg。
###		
###		pthread->self_kstack -= sizeof(struct intr_stack);	先预留中断使用栈的空间,可见thread.h中定义的结构
###		pthread->self_kstack -= sizeof(struct thread_stack);	再留出线程栈空间,可见thread.h中定义
###
###		##############################################################################################
###		##	struct thread_stack {                                          							##
###		##		uint32_t ebp;    <---低地址                                          				##
###		##		uint32_t ebx;                                              							##
###		##		uint32_t edi;                                              							##
###		##		uint32_t esi;                                              							##
###		##	                                                               							##
###		##		void (*eip) (thread_func* func, void* func_arg);           							##
###		##		void (*unused_retaddr);                                    							##
###		##		thread_func* function;	由Kernel_thread所调用的函数名，類型為void (*function)(void*)##
###		##		void* func_arg;    	  	由Kernel_thread所调用的函数所需的参数						##
###		##	};                                                              						##
###		##############################################################################################
###
###		##############################################################################################
###		##	struct intr_stack {                                                           			##
###		##		uint32_t vec_no;	 kernel.S 宏VECTOR中push %1压入的中断号    						##
###		##		uint32_t edi;                                                                 		##
###		##		uint32_t esi;                                                                 		##
###		##		uint32_t ebp;                                                                 		##
###		##		uint32_t esp_dummy;	 虽然pushad把esp也压入,但esp是不断变化的,所以会被popad忽略		##
###		##		uint32_t ebx;                                                                 		##
###		##		uint32_t edx;                                                                 		##
###		##		uint32_t ecx;                                                                 		##
###		##		uint32_t eax;                                                                 		##
###		##		uint32_t gs;                                                                  		##
###		##		uint32_t fs;                                                                  		##
###		##		uint32_t es;                                                                  		##
###		##		uint32_t ds;                                                                  		##
###		##		                                                                              		##
###		##		以下由cpu从低特权级进入高特权级时压入                                         		##
###		##		uint32_t err_code;	err_code会被压入在eip之后                                 		##
###		##		void (*eip) (void);                                                           		##
###		##		uint32_t cs;                                                                  		##
###		##		uint32_t eflags;                                                              		##
###		##		void* esp;                                                                    		##
###		##		uint32_t ss; 	    <---高地址                                                		##
###		##	};                                                                            			##
###		##############################################################################################
###		
###		struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
###		把減完兩次的self_kstack 強制轉化 為執行緒堆疊指標(struct thread_stack*)，賦予struct thread_stack* kthread_stack，
###		
###		kthread_stack->eip = kernel_thread; 把kernel_thread函數(定義在本檔最上面)的地址給eip
###		kthread_stack->function = function; 把function給函數指標(名字也叫function)
###		kthread_stack->func_arg = func_arg; 賦予void function(void*)的參數
###		kthread_stack->ebp = kthread_stack->ebx = kthread_stack->esi = kthread_stack->edi = 0; 剩下的暫存器全歸0
*/