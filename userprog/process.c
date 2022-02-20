#include "process.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "thread.h"    
#include "list.h"    
#include "tss.h"    
#include "interrupt.h"
#include "string.h"
#include "console.h"

extern void intr_exit(void);

/* 构建用户进程初始上下文信息 */
void start_process(void* filename_) {
    void* function = filename_;
    struct task_struct* cur = running_thread();
    cur->self_kstack += sizeof(struct thread_stack);  //跨过thread_stack,指向intr_stack
    struct intr_stack* proc_stack = (struct intr_stack*)cur->self_kstack;//可以不用定义成结构体指针	 
    proc_stack->edi = proc_stack->esi = proc_stack->ebp = proc_stack->esp_dummy = 0;
    proc_stack->ebx = proc_stack->edx = proc_stack->ecx = proc_stack->eax = 0;
    proc_stack->gs = 0;		 // 不太允许用户态直接访问显存资源,用户态用不上,直接初始为0
    proc_stack->ds = proc_stack->es = proc_stack->fs = SELECTOR_U_DATA;
    proc_stack->eip = function;	 // 待执行的用户程序地址
    proc_stack->cs = SELECTOR_U_CODE;
    proc_stack->eflags = (EFLAGS_IOPL_0 | EFLAGS_MBS | EFLAGS_IF_1);
    proc_stack->esp = (void*)((uint32_t)get_a_page(PF_USER, USER_STACK3_VADDR) + PG_SIZE) ;
    proc_stack->ss = SELECTOR_U_DATA; 
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (proc_stack) : "memory");
}

/* 击活页表 */
void page_dir_activate(struct task_struct* p_thread) {
    /********************************************************
     * 执行此函数时,当前任务可能是线程。
     * 之所以对线程也要重新安装页表, 原因是上一次被调度的可能是进程,
     * 否则不恢复页表的话,线程就会使用进程的页表了。
     ********************************************************/

    /* 若为内核线程,需要重新填充页表为0x100000 */
    uint32_t pagedir_phy_addr = 0x100000;  // 默认为内核的页目录物理地址,也就是内核线程所用的页目录表
    if (p_thread->pgdir != NULL)	{    // 用户态进程有自己的页目录表
        pagedir_phy_addr = addr_v2p((uint32_t)p_thread->pgdir);
    }

    /* 更新页目录寄存器cr3,使新页表生效 */
    asm volatile ("movl %0, %%cr3" : : "r" (pagedir_phy_addr) : "memory");
}

/* 激活线程或进程的页表,更新tss中的esp0为进程的特权级0的栈 */
void process_activate(struct task_struct* p_thread) {
    ASSERT(p_thread != NULL);
    /* 激活该进程或线程的页表 */
    page_dir_activate(p_thread);

    /* 内核线程特权级本身就是0特权级,处理器进入中断时并不会从tss中获取0特权级栈地址,故不需要更新esp0 */
    if (p_thread->pgdir) {
        /* 更新该进程的esp0,用于此进程被中断时保留上下文 */
        update_tss_esp(p_thread);
    }
}

/* 创建页目录表,将当前页表的表示内核空间的pde复制,
 * 成功则返回页目录的虚拟地址,否则返回-1 */
uint32_t* create_page_dir(void) {

    /* 用户进程的页表不能让用户直接访问到,所以在内核空间来申请 */
    uint32_t* page_dir_vaddr = get_kernel_pages(1);
    if (page_dir_vaddr == NULL) {
        console_put_str("create_page_dir: get_kernel_page failed!");
        return NULL;
    }

    /************************** 1  先复制页表  *************************************/
    /*  page_dir_vaddr + 0x300*4 是内核页目录的第768项 */
    memcpy((uint32_t*)((uint32_t)page_dir_vaddr + 0x300*4), (uint32_t*)(0xfffff000+0x300*4), 1024);
    /*****************************************************************************/

    /************************** 2  更新页目录地址 **********************************/
    uint32_t new_page_dir_phy_addr = addr_v2p((uint32_t)page_dir_vaddr);
    /* 页目录地址是存入在页目录的最后一项,更新页目录地址为新页目录的物理地址 */
    page_dir_vaddr[1023] = new_page_dir_phy_addr | PG_US_U | PG_RW_W | PG_P_1;
    /*****************************************************************************/
    return page_dir_vaddr;
}

/* 创建用户进程虚拟地址位图 */
void create_user_vaddr_bitmap(struct task_struct* user_prog) {
    user_prog->userprog_vaddr.vaddr_start = USER_VADDR_START;
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000 - USER_VADDR_START) / PG_SIZE / 8 , PG_SIZE);
    user_prog->userprog_vaddr.vaddr_bitmap.bits = get_kernel_pages(bitmap_pg_cnt);
    user_prog->userprog_vaddr.vaddr_bitmap.btmp_bytes_len = (0xc0000000 - USER_VADDR_START) / PG_SIZE / 8;
    bitmap_init(&user_prog->userprog_vaddr.vaddr_bitmap);
}
/*
###需要注意:
###DIV_ROUND_UP為巨集函數，定義在global.h，目的在向上取整(無條件進位)，
###向上取整的公式為:(a+b-1)/b，如14/4=3...2，向上取整為(14+3)/4...1，
###即如果商是3.XXX，則把商變成4。
###
###描述 核心池、使用者池、核心所用的虛擬地址的點陣圖 設置在低端1MB的0xc009a000以上的位置，
###而使用者所用的虛擬地址的點陣圖 設置在 核心池 裡面。
*/

/* 创建用户进程 */
void process_execute(void* filename, char* name) { 
    /* pcb内核的数据结构,由内核来维护进程信息,因此要在内核内存池中申请 */
    struct task_struct* thread = get_kernel_pages(1);
    init_thread(thread, name, default_prio); 
    create_user_vaddr_bitmap(thread);
    thread_create(thread, start_process, filename);//start_process(filename)
    thread->pgdir = create_page_dir();
	
//~~~~~~~~~~~~~~~~~~第12章e~~~~~~~~~~~~~~~~~~~~~~~
	block_desc_init(thread->u_block_desc);
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);

    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);
    intr_set_status(old_status);
}
/*
###需要注意:
###	由於CPU不允許直接由高特權等級傳遞到低特權等級，所以用退中斷的偏門方式來騙過CPU，以此傳遞特權等級。
###	
###
###	目前為使用者處理程序就建了四種分頁:
###
###	1.使用者處理程序執行緒的PCB(在process_execute函數內分配，在物理地址位於核心池內，基底地址為"核心"虛擬地址，佔一分頁)、
###
###	2.放 使用者處理程序用的虛擬位址的 點陣圖的 分頁
###   (在process_execute函數內的create_user_vaddr_bitmap函數內分配，
###   物理地址位於核心池內，基底地址為"核心"虛擬地址，大小不只一個分頁，基底地址即第一個分頁的基底位址)，
###
###	3.使用者處理程序用的 分頁目錄表
###   (在process_execute函數內的create_page_dir函數內分配，
###   物理地址位於核心池內，基底地址為"核心"虛擬地址，佔一分頁，
###   因為不能直接讓使用者訪問到分頁目錄表，所以該表的物理地址位在 核心池 中)，
###
###	4.使用者處理程序的堆疊空間 用的分頁
###   (交棒後在switch_to函數內pop到start_process函數時，在start_process函數內分配，物理地址位於使用者池內，基底地址為"使用者"虛擬地址，佔一分頁)，
###	  C程式下的 使用者 記憶體分配的規則，由低地址到高地址依序是 程式碼段、初始化資料、未初始化資料、堆疊(包含指令行參數和環境變數)，
###	  使用者處理程序的堆疊空間是 C程式下的 使用者 記憶體分配 的 一部份，所以分配到的物理地址是在 使用者池 內。
###
### 以上1. 2. 3.分頁皆是交棒給使用者前就要配置好的，為 核心 替 使用者 "先"配置的分頁，填表皆是填在最原始的兩個表內，
###	4.是 使用者自己要用的，在交棒時，進入switch_to前，會先進入process_activate函數，把cr3更新為 新的分頁目錄表的基底物理位址，
### 所以填表是填在 新的分頁目錄表 裡，此 新的分頁目錄表 的高四分之一處 複製了 原本分頁目錄表 的內容，
### 新的分頁目錄表的低四分之三處是使用者用的，還沒填任何東西，所以會先 創一個新的分頁表 ，該分頁表位於 使用者核心池內，然後把數值填入新分頁表。
*/
