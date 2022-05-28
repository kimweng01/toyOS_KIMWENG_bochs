#include "memory.h"
#include "bitmap.h"
#include "stdint.h"
#include "global.h"
#include "debug.h"
#include "print.h"
#include "string.h"
#include "sync.h"
#include "interrupt.h"

//#define PG_SIZE 4096 ##已定義在global.h

/***************  位图地址 ********************
* 因为0xc009f000是内核主线程栈顶，0xc009e000是内核主线程的pcb.
* 一个页框大小的位图可表示128M内存, 位图位置安排在地址0xc009a000,
* 这样本系统最大支持4个页框的位图,即512M */
#define MEM_BITMAP_BASE 0xc009a000
/*************************************/

#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22)
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12)

/* 0xc0000000是内核从虚拟地址3G起. 0x100000意指跨过低端1M内存,使虚拟地址在逻辑上连续 */
#define K_HEAP_START 0xc0100000



//#################################################################################################################################
/* 内存池结构,生成两个实例用于管理内核内存池和用户内存池 */
struct pool {
    struct bitmap pool_bitmap;	 	// 本内存池用到的位图结构,用于管理物理内存
    uint32_t phy_addr_start;	 	// 本内存池所管理物理内存的起始地址
    uint32_t pool_size;		 		// 本内存池字节容量
	
//~~~~~~~~~~~~~~~~~第11章c~~~~~~~~~~~~~~~~~~~~~~~
	struct lock lock;
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
};

//~~~~~~~~~~~~~~~~~第12章e~~~~~~~~~~~~~~~~~~~~~~~
/* 内存仓库arena元信息 */
struct arena {
    struct mem_block_desc* desc;	 // 此arena关联的mem_block_desc
    /* large为ture时,cnt表示的是页框数。
     * 否则cnt表示空闲mem_block数量 */
    uint32_t cnt;
    bool large;		   
};
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//~~~~~~~~~~~~~~~~~第12章e~~~~~~~~~~~~~~~~~~~~~~~
struct mem_block_desc k_block_descs[DESC_CNT];	// 内核内存块描述符数组
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
struct pool kernel_pool, user_pool; // 生成内核内存池和用户内存池
struct virtual_addr kernel_vaddr;	// 此结构是用来给内核分配虚拟地址



//#################################################################################################################################
/* 为malloc做准备 */
void block_desc_init(struct mem_block_desc* desc_array) {				   
   uint16_t desc_idx, block_size = 16;

   /* 初始化每个mem_block_desc描述符 */
   for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
      desc_array[desc_idx].block_size = block_size;

      /* 初始化arena中的内存块数量 */
      desc_array[desc_idx].blocks_per_arena = (PG_SIZE - sizeof(struct arena)) / block_size;	  

      list_init(&desc_array[desc_idx].free_list);

      block_size *= 2;         // 更新为下一个规格内存块
   }
}

/* 在pf表示的虚拟内存池中申请pg_cnt个虚拟页,
 * 成功则返回虚拟页的起始地址, 失败则返回NULL */
static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt) {
   int vaddr_start = 0, bit_idx_start = -1;
   uint32_t cnt = 0;
   
   if (pf == PF_KERNEL) { //##如果是核心內存池
      bit_idx_start  = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
      if (bit_idx_start == -1) {
		return NULL;
      }
      while(cnt < pg_cnt) {
		bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
      }
      vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
   } 
   else {
   //~~~~~~~~~~~~~~~~~~第11章c~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	  struct task_struct* cur = running_thread();
      bit_idx_start  = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
      if (bit_idx_start == -1) {
		return NULL;
	  }
	  while(cnt < pg_cnt) {
		bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
      }
      vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;

   /* ###(0xc0000000 - PG_SIZE)做为用户3级栈已经在start_process被分配 */
      ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
   //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   }
   return (void*)vaddr_start;
}


//=================================================================================
/* 得到虚拟地址vaddr对应的pte指针*/
uint32_t* pte_ptr(uint32_t vaddr) {
   /* 先访问到页表自己 + \
    * 再用页目录项pde(页目录内页表的索引)做为pte的索引访问到页表 + \
    * 再用pte的索引做为页内偏移*/
   uint32_t* pte = (uint32_t*)(0xffc00000 + \
	 ((vaddr & 0xffc00000) >> 10) + \
	 PTE_IDX(vaddr) * 4);
   return pte;
}

/* 得到虚拟地址vaddr对应的pde的指针 */
uint32_t* pde_ptr(uint32_t vaddr) {
   /* 0xfffff是用来访问到页表本身所在的地址 */
   uint32_t* pde = (uint32_t*)((0xfffff000) + PDE_IDX(vaddr) * 4);
   return pde;
}


//=================================================================================
/* 在m_pool指向的物理内存池中分配1个物理页,
 * 成功则返回页框的物理地址,失败则返回NULL */
static void* palloc(struct pool* m_pool) {
   /* 扫描或设置位图要保证原子操作 */
   int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1); // 找一个物理页面
   if (bit_idx == -1 ) {
      return NULL;
   }
   bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);       // 将此位bit_idx置1
   uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);
   return (void*)page_phyaddr;
}


//=================================================================================
/* 页表中添加虚拟地址_vaddr与物理地址_page_phyaddr的映射 */
static void page_table_add(void* _vaddr, void* _page_phyaddr) {
   uint32_t vaddr = (uint32_t)_vaddr, page_phyaddr = (uint32_t)_page_phyaddr;
   uint32_t* pde = pde_ptr(vaddr);
   uint32_t* pte = pte_ptr(vaddr);

/************************   注意   *************************
 * 执行*pte,会访问到空的pde。所以确保pde创建完成后才能执行*pte,
 * 否则会引发page_fault。因此在*pde为0时,*pte只能出现在下面else语句块中的*pde后面。
 * *********************************************************/
   /* 先在页目录内判断目录项的P位，若为1,则表示该表已存在 */
   if (*pde & 0x00000001) {	 // 页目录项和页表项的第0位为P,此处判断目录项是否存在
      ASSERT(!(*pte & 0x00000001)); //*pte & 0x00000001 == true會ASSERT

      if (!(*pte & 0x00000001)) {   // 只要是创建页表,pte就应该不存在,多判断一下放心
		*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);    // US=1,RW=1,P=1
      } 
	  else {			    //应该不会执行到这，因为上面的ASSERT会先执行。
		PANIC("pte repeat");
		*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);      // US=1,RW=1,P=1
      }
   } 
   else {			    // 页目录项不存在,所以要先创建页目录再创建页表项.
      /* 页表中用到的页框一律从内核空间分配 */
      uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);

      *pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);

      /* 分配到的物理页地址pde_phyaddr对应的物理内存清0,
       * 避免里面的陈旧数据变成了页表项,从而让页表混乱.
       * 访问到pde对应的物理地址,用pte取高20位便可.
       * 因为pte是基于该pde对应的物理地址内再寻址,
       * 把低12位置0便是该pde对应的物理页的起始*/
      memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);
         
      ASSERT(!(*pte & 0x00000001));
      *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);      // US=1,RW=1,P=1
   }
}


//=================================================================================
/* 分配pg_cnt个页空间,成功则返回起始虚拟地址,失败时返回NULL */
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
   ASSERT(pg_cnt > 0 && pg_cnt < 3840);
/***********   malloc_page的原理是三个动作的合成:   ***********
      1通过vaddr_get在虚拟内存池中申请虚拟地址
      2通过palloc在物理内存池中申请物理页
      3通过page_table_add将以上得到的虚拟地址和物理地址在页表中完成映射
***************************************************************/
   void* vaddr_start = vaddr_get(pf, pg_cnt);
   if (vaddr_start == NULL) {
      return NULL;
   }

   uint32_t vaddr = (uint32_t)vaddr_start, cnt = pg_cnt;
   struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;

   /* 因为虚拟地址是连续的,但物理地址可以是不连续的,所以逐个做映射*/
   while (cnt-- > 0) {
      void* page_phyaddr = palloc(mem_pool);
      if (page_phyaddr == NULL) {  // 失败时要将曾经已申请的虚拟地址和物理页全部回滚，在将来完成内存回收时再补充
		return NULL;
      }
      page_table_add((void*)vaddr, page_phyaddr); // 在页表中做映射 
      vaddr += PG_SIZE;		 // 下一个虚拟页
   }
   return vaddr_start;
}


//=================================================================================
/* 从内核物理内存池中申请pg_cnt页内存,成功则返回其虚拟地址,失败则返回NULL */
void* get_kernel_pages(uint32_t pg_cnt) {
   lock_acquire(&kernel_pool.lock);	//##<======第11章b後新增，
   
   void* vaddr =  malloc_page(PF_KERNEL, pg_cnt);
   if (vaddr != NULL) {	   // 若分配的地址不为空,将页框清0后返回
      memset(vaddr, 0, pg_cnt * PG_SIZE);
   }
   
   lock_release(&kernel_pool.lock); //##<======第11章b後新增，
   return vaddr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~第11章b~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 在用户空间中申请4k内存,并返回其虚拟地址 */
void* get_user_pages(uint32_t pg_cnt) {
   lock_acquire(&user_pool.lock);
   void* vaddr = malloc_page(PF_USER, pg_cnt);
   memset(vaddr, 0, pg_cnt * PG_SIZE);
   lock_release(&user_pool.lock);
   return vaddr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~第15章g~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 将地址vaddr与pf池中的物理地址关联,仅支持一页空间分配
void* get_a_page(enum pool_flags pf, uint32_t vaddr) {
   struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
   lock_acquire(&mem_pool->lock);

   先将虚拟地址对应的位图置1
   struct task_struct* cur = running_thread();
   int32_t bit_idx = -1;

   若当前是用户进程申请用户内存,就修改用户进程自己的虚拟地址位图
   if (cur->pgdir != NULL && pf == PF_USER) {
      bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
      ASSERT(bit_idx > 0);
      bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);

   } 
   else if (cur->pgdir == NULL && pf == PF_KERNEL){
   如果是内核线程申请内核内存,就修改kernel_vaddr.
      bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
      ASSERT(bit_idx > 0);
      bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
   } 
   else {
      PANIC("get_a_page:not allow kernel alloc userspace or user alloc kernelspace by get_a_page");
   }

   void* page_phyaddr = palloc(mem_pool);
   if (page_phyaddr == NULL) {
      return NULL;
   }
   page_table_add((void*)vaddr, page_phyaddr); 
   lock_release(&mem_pool->lock);
   return (void*)vaddr;
}
*/

void* get_a_page(enum pool_flags pf, uint32_t vaddr) {
   struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
   lock_acquire(&mem_pool->lock);

   /* 先将虚拟地址对应的位图置1 */
   struct task_struct* cur = running_thread();
   int32_t bit_idx = -1;

/* 若当前是用户进程申请用户内存,就修改用户进程自己的虚拟地址位图 */
   if (cur->pgdir != NULL && pf == PF_USER && vaddr < 0x8048000) {
      bit_idx = vaddr / PG_SIZE;
      ASSERT(bit_idx >= 0);//<===============
      bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
   } else if (cur->pgdir != NULL && pf == PF_USER) {
      bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
      ASSERT(bit_idx >= 0);//<===============
      bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
   } else if (cur->pgdir == NULL && pf == PF_KERNEL){
/* 如果是内核线程申请内核内存,就修改kernel_vaddr. */
      bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
      ASSERT(bit_idx >= 0);//<===============
      bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
   } else {
      PANIC("get_a_page:not allow kernel alloc userspace or user alloc kernelspace by get_a_page");
   }

   void* page_phyaddr = palloc(mem_pool);
   if (page_phyaddr == NULL) {
      lock_release(&mem_pool->lock);
      return NULL;
   }
   page_table_add((void*)vaddr, page_phyaddr);
   lock_release(&mem_pool->lock);
   return (void*)vaddr;
}
	/*
	#本get_a_page需要大改!!!
	#使用者處理程序用的堆疊會放在0x8004800處以上的位置，
	#而app的code會放置在低於0x8004800處，
	#所以多新增可以處理 低於0x8004800處的 if (cur->pgdir != NULL && pf == PF_USER && vaddr < 0x8048000)
	#此外，bit_idx應該是從0開始算，所以三個ASSERT改成SSERT(bit_idx >= 0)。
	*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//~~~~~~~~~~~~~~~~~~~~~~~~第15章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 安装1页大小的vaddr,专门针对fork时虚拟地址位图无须操作的情况 */
void* get_a_page_without_opvaddrbitmap(enum pool_flags pf, uint32_t vaddr) {
   struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
   lock_acquire(&mem_pool->lock);
   void* page_phyaddr = palloc(mem_pool);
   if (page_phyaddr == NULL) {
      lock_release(&mem_pool->lock);
      return NULL;
   }
   page_table_add((void*)vaddr, page_phyaddr); 
   lock_release(&mem_pool->lock);
   return (void*)vaddr;
}

/* 得到虚拟地址映射到的物理地址 */
uint32_t addr_v2p(uint32_t vaddr) {
   uint32_t* pte = pte_ptr(vaddr);
/* (*pte)的值是页表所在的物理页框地址,
 * 去掉其低12位的页表项属性+虚拟地址vaddr的低12位 */
   return ((*pte & 0xfffff000) + (vaddr & 0x00000fff));
}

//=================================================================================
/*	###統整:
	###	需要注意:
	###	1.目前已經開啟分頁模式，也就是CPU看到的都是虛擬地址，需要做三次映射才會到物理地址上。
	###	  因此物理地址只能填在分頁目錄表或分頁表裡，
	###	  讓CPU用 看到的地址 去分頁目錄表或分頁表裡找 物理地址 去作映射，
	###	  物理地址不能直接給CPU!
	###	2.從第11章開始，只要是分配記憶體有關的函數都要用PV包起來，
	###	  涉及到分配記憶體的函數，都要用PV包起來，
	###   如果沒用PV包起來，假如A在點陣圖找到cnt個0的位置，
	###   要把這些位置置1時，突然時間到把棒交出去給B了，
	###   現在B也要分配記憶體，把上一棒在點陣圖找到cnt個0的位置都置1，
	###   等到棒交回來時，A已經通過 在點陣圖找到cnt個0的位置 之檢查了，
	###   也會把剛剛B在點陣圖的位置也置1，即A和B用到點陣圖的同一位置了，
	###   如果該點陣圖是描述虛擬位址的，會共用到同一個虛擬位址，
	###   如果該點陣圖是描述核心池或使用者池的，會共用到同一個物理地址(更嚴重)，
	###   所以涉及到分配記憶體的函數，都要用PV包起來!
	###	
	###	還在loader.S中的時候:
	###	分頁目錄表全歸0，
	###	分頁目錄表的 高四分之一處的 第0個位置(從0開始算)寫第0個分頁的基底位址，
	###	分頁目錄表的 高四分之一處的 第1個位置(從0開始算)寫第1個分頁的基底位址...
	###	分頁目錄表的 高四分之一處的 最後一個位置寫 分頁目錄表自己的 基底位址，即指向分頁目錄表自己，
	###	最後往 分頁目錄表的高四分之一處 寫256個 分頁表的位址，第0個分頁表的低四分之一處 寫256個 分頁的位址。
	###	
	###	核心的虛擬位址都要是0xcXXXXXXX開頭，即 高10位元 拆出來都對應到的 分頁目錄表高四分之一處 的位置，
	###	小於0xcXXXXXXX的地址的 高10位元 拆出來會對應到 分頁目錄表的 低四分之三處 位置，
	###	這些都是 使用者 要用的用的 虛擬位址。
	###	
	###	而虛擬位址小於0xc0100000的 中10位元 拆出來會對應到 分頁表內最一開始在loader被寫256次的地方，
	###	所以虛擬位址(vaddr)要從0xc0100000開始算起。
	###	
	###	點陣圖有三個，
	###	核心的點陣圖檢查核心內存池(在實體記憶體0x200000以上的位置)的每個 分頁 有沒有被使用，
	###	使用者的點陣圖檢查使用者內存池(在實體記憶體0x200000以上的位置)的每個 分頁 有沒有被使用，
	###	虛擬位址的點陣圖檢查該虛擬位址(從0xc0100000開始算)有沒有被使用，
	###	三個點陣圖使用前都會先歸0。
	###	三個點陣圖使用前都會先歸0。
	###
	###
	###	vaddr_get函數在找pg_cnt個沒被使用的虛擬位址(該虛擬位址指向分頁的基底位址，所以一定要是000結尾)，
	###	找到的話把點陣圖的pg_cnt個位元置1，
	###	返回這 "pg_cnt個"沒被使用的 虛擬位址的 "第0個"的 "虛擬地址"，
	###	該地址的計算方法為 找到的"pg_cnt個"沒被使用的 "第0個"的 "點陣圖位址"，
	###	把這"點陣圖位址"乘4K(點陣圖的一個位元代表1個分頁大小) 再+虛擬位址的起始地址="第0個"的 "虛擬地址"。
	###	vaddr_get只是在檢查該 虛擬位址 有沒有被使用而已，沒被使用就會被分配出去，不觸及內存池!
	###	
	###	pte_ptr函數的目的在獲得 指向vaddr的指標(黑色箭頭)的 存放"位址" (黑色箭頭畫在筆記照相記憶體圖裡)，
	###	意即要取得 從vaddr拆出的中10位 要填的"位址"，
	###	此指標的構建方法為:
	###	在高10位元寫10個1(0xffc00000)，
	###	vaddr原本高10位元的寫到中10位元處，
	###	vaddr原本高10位元的寫到低12位元處並乘以4(低12位元處理器不會自動乘4)，
	###	在高10位元寫10個1，等於對應到分頁目錄表裡最後一個項目，該項目指向自己，
	###	等於最一開始多打轉了一圈，處理器只剩下兩次映射的機會，最後的結果就是存黑色箭頭(指標)的"位址"。
	###	
	###	pde_ptr函數的目的在獲得 指向vaddr的黑色箭頭的 紅色箭頭(看筆記照相圖) 的 存放"位址"，
	###	原理跟pte_ptr函數差不多，只是在高20位元全部寫1，vaddr原本的高10位元寫到低12位元並乘4，
	###	等於最一開始多打轉了兩圈，處理器只剩下一次映射的機會，最後的結果就是存紅色箭頭(指標)的"位址"。
	###	
	###	palloc函數等於在核心存池或使用者內存池尋找1個能用的 分頁，
	###	找到的話把該位元置1，並回傳內存池內的 該分頁表的 基底物理位址。
	###	該地址的計算方法為 找到的"1個"沒被使用的 "XX物理點陣圖的位"，
	###	把這"XX物理點陣圖的位"乘4K(點陣圖的一個位元代表1個分頁大小) 再+XX物理位址的起始地址=找到的"1個"沒被使用的 物理地址。
	###
	### 虛擬地址很自由，可以連續，用多大都無所謂，
	### 物理地址即 核心池 和 使用者池 各不到16MB，大多都不連續
	### 如果物理地址用完，導致palloc函數回傳NULL，表示已經沒有能用的物理空間了。
	###	
	###	page_table_add函數目的在 虛擬地址_vaddr 與 物理地址_page_phyaddr 作映射，
	###	(該物理地址是分頁的物理地址，所以一定是像0xXXXXX000三個0作結為)
	###	意思是要確保把虛擬地址(_vaddr)給CPU映射3次後的結果能落到正確的物理地址(_page_phyaddr)上，
	###	如果分頁目錄表和分頁表內沒有出現該有的值要填寫進去，這會用到pte_ptr函數和pde_ptr函數，
	###	要先檢查_vaddr映射到分頁目錄表內的內容是否存在，即判斷紅色箭頭是否存在，
	###	如果紅色箭頭存在(*pde & 0x00000001 == true)，
	###		會檢查_vaddr映射到的分頁表內的內容(分頁)是否存在，即檢查黑色箭頭是否存在，
	###		如果存在(*pte & 0x00000001 == true)，
	###			等於是你要創造的分頁已經存在了，在malloc_page函數中，
	###			只要一找到pg_cnt個虛擬位址就會為它們填表，並為虛擬位址的點陣圖填1。
	###			所以不可能發生找到的虛擬位址(虛擬位址點陣圖為0)的數值已經填入表內的狀況，
	###			所以會ASSERT。
	###		如果不存在(*pte & 0x00000001 == false)，
	###			則直接把物理地址跟PG_US_U | PG_RW_W | PG_P_1作OR運算並寫進去分頁表內，
	###			這樣CPU映射兩次完到分頁表內的時候，可以直接拿分頁表內的地址映射到物理地址上(第三次)。
	###			分頁內的內容要不要歸0交給類似get_kernel_pages的這種外部的函數來決定。
	###	如果連紅色箭頭也不存在(*pde & 0x00000001 == false)的話，那就得先創造紅色箭頭再創黑色箭頭，
	###	因為在loader.S的時候只要有創一個 分頁表 就會讓紅色箭頭指向它，
	###	所以表示說現在連紅色箭頭都沒有表示連個 分頁表 也都沒有，
	###	需要在原本放 分頁 的地方額外讓出一個放 分頁表 的大小的空間(分頁表大小=分頁大小)給分頁表，
	###	即用創 分頁 的方式創一個 分頁表。
	###		用palloc函數創造一個新的分頁表，回傳值為 新分頁表的 基底物理位址，
	###		把 新分頁表的 基底物理位址跟PG_US_U | PG_RW_W | PG_P_1作OR運算並寫進去分頁目錄表內，
	###		然後新創的分頁表裡面要全歸0，但是"CPU要讀地址"去歸0，
	###		也就是CPU這地址是要給CPU讀的，不是寫進XXX表裡的，所以要給CPU"虛擬地址"去歸0。
	###		分頁表的 虛擬起始位址 就是把原本 存黑色箭頭的地址 從0xXXXXXXXX 變成 0xXXXXX000，
	###		把該地址(0xXXXXX000)給mem_set，要歸0的數量=1個頁表的大小=4K，這樣就可以全歸0了。
	###	需要注意，一定要先看紅色箭頭在不在再去找黑色箭頭，要不然直接找黑色箭頭結果沒有紅色箭頭就會page_fault!
	###		
	###	malloc_page函數目的是要分配pg_cnt個 分頁，
	###	然後返回申請到的 配pg_cnt個分頁的 第0個分頁的 虛擬地址，該虛擬地址經過三次映射後會對應到物理地址，
	###	也就是兩個表裡面該寫的內容都要填進去，等於要用到以上所有的函數。
	###	首先要找到pg_cnt個能用的虛擬位址，要用vaddr_get函數獲得pg_cnt個沒被使用的 "第0個"的 "虛擬地址"，
	###	然後進入while迴圈，
	###		用palloc函數為pg_cnt個分頁一個一個尋找能用的分頁，得到一個分頁的 物理地址。
	###		然後用page_table_add把該填的 物理地址的數值 填到兩個表內
	###		然後 vaddr+分頁大小(vaddr是 "分頁" 的 虛擬地址，所以vaddr的下一個地址是+分頁大小)， 
	###		再去為下一個分頁尋找能用的 物理地址 並填表。
	###	最後返回pg_cnt個分頁的 第0個分頁的 虛擬位址。
	###	需要注意的是，虛擬地址是連續的，可以在vaddr_get直接給pg_cnt傳進去函數一次找完，
	###	而物理地址可以是不連續的需要在while迴圈內 一個分頁一個分頁 找。	
	###
	###	get_kernel_pages函數是本memory.c的最終目的，要從核心的物理內存池申請pg_cnt個分頁，
	###	需調用malloc_page函數，再把申請到的pg_cnt個分頁全部清0。	
	###
	###~~~~~~~~~~~~~~~~~~~~~~~~第11章b~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	###	get_user_pages函數跟get_kernel_pages函數差不多，不過該函數是要從 使用者的 物理內存池申請pg_cnt個分頁，
	###
	###
	###	get_a_page函數目的跟malloc_page函數類似，
	###	但不同的是get_a_page函數的虛擬地址不是函數內部幫忙找的，而是你自己指定的(會做為參數傳進get_a_page函數內)，而且只能分配一個分頁，
	###	首先用running_thread()獲取目前PCB的基底位址(即struct task_struct*)，將該值賦予cur，
	###
	###	看cur->pgdir是不是等於NULL 及 看pf等於什麼 判斷目前的指定的 虛擬地址 是 使用者 還是 核心 ，
	###	如果是 使用者 的 虛擬地址 就要去使用者的bitmap(在process_execute內會用create_user_vaddr_bitmap函數把該bitmap的物理位置設在核心池)，
	###	如果是 核心   的 虛擬地址 就要去使用者的bitmap(該bitmap位在0xc00XXXXX處)，
	###
	###	如果是使用者的話，用(vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE 的方式來得知指定的 使用者的虛擬地址位在 使用者虛擬位址點陣圖中 的第幾位，
	###	把 使用者虛擬位址點陣圖 的那一位置1，
	###	(使用者的 struct virtual_addr userprog_vaddr 設置在 struct task_struct 內)，
	###	如果是核心的話，用(vaddr - kernel_vaddr.vaddr_start) / PG_SIZE 的方式來得知指定的 核心的虛擬地址位在 核心虛擬位址點陣圖中 的第幾位，
	###	把 核心虛擬位址點陣圖 的那一位置1，
	###	(核心的 struct virtual_addr 設置在 0xc00XXXXX 內)，
	###	
	###	接著用palloc函數為 使用者 或 核心 在 使用者池 或 核心池 尋找一個能用的 分頁，
	###	找到後回傳該分頁的 物理基底位址，
	###	接著用page_table_add函數把 指定的虛擬地址 與 剛用palloc函數找到的物理地址 做映射，把該填的數值填到兩個表(原本的)內，
	###	最後返回指定的虛擬地址(若palloc函數沒找到能用的物理地址會回傳NULL)。	
	###
	###
	###	addr_v2p函數目的在求 指定的虛擬地址(會做為參數傳進addr_v2p函數內) 經過三次映射 最終對應到的 "物理地址"，
	###	方法為先用pte_ptr函數獲得 指向vaddr的指標(黑色箭頭)的 存放"位址"(看筆記照相)，把該值給pte，
	###	再把得到的pte加上星號(即變成*pte)後 即可獲得 "黑色箭頭所指向的" 物理地址 ，
	###	把*pte和0xfffff000做and(把位指數值後面的007去掉) 加上 指定的虛擬地址(vaddr)的低12位 即可獲得 指定的虛擬地址 經過三次映射 最終對應到的 "物理地址"。	*/

//#################################################################################################################################
/* 初始化内存池 */
static void mem_pool_init(uint32_t all_mem) {
    put_str("   mem_pool_init start\n");
	
    uint32_t page_table_size = PG_SIZE * 256;	  		// 页表大小= 1页的页目录表+第0和第768个页目录项指向同一个页表+
/**==>##page_table_size為頁表所佔的總大小，單位為byte**/
	//###所有的頁表佔PG_SIZE * 256個地址				// 第769~1022个页目录项共指向254个页表,共256个页框
    //###因為頁表目錄最後一個指向自己，所以自己也算一個頁表，因此總共有256個頁表，然後256個頁表從0x100000開始算(看筆記照相圖!)
	
	uint32_t used_mem = page_table_size + 0x100000;	  	// 0x100000为低端1M内存
/**==>##頁表所佔的總大小(單位為byte)+記憶體最一開始的1MB=被使用的記憶體總大小=used_mem，單位為byte**/
    
	uint32_t free_mem = all_mem - used_mem;
/**==>##查到的能用的總記憶體大小(=all_mem，應為32MB)-被使用的記憶體總大小(used_mem)=能自由使用的記憶體大小=free_mem**/

    uint16_t all_free_pages = free_mem / PG_SIZE;		// 1页为4k,不管总内存是不是4k的倍数,
/**==>##能自由使用的記憶體大小(free_mem)除以一個頁表的大小=能自由使用的頁表數量=all_free_pages**/
	//###剩下的記憶體可以分配free_mem / PG_SIZ的頁表
    // 对于以页为单位的内存分配策略，不足1页的内存不用考虑了。
 
//----------------------------------------------------- 
	uint16_t kernel_free_pages = all_free_pages / 2;
/**==>##把能自由使用的頁表大小(all_free_pages)的一半分給核心，kernel_free_pages為核心能自由使用的頁表數量**/

    uint16_t user_free_pages = all_free_pages - kernel_free_pages;
/**==>##把能自由使用的頁表大小(all_free_pages)未分給核心的另一半給使用者，kernel_free_pages為使用者能自由使用的頁表數量**/
	//###把"用剩下的記憶體"分配到的頁表的"一半"給使用者使用

//----------------------------------------------------- 
    /* 为简化位图操作，余数不处理，坏处是这样做会丢内存。
    好处是不用做内存的越界检查,因为位图表示的内存少于实际物理内存*/
    uint32_t kbm_length = kernel_free_pages / 8;		// Kernel BitMap的长度,位图中的一位表示一页,以字节为单位
/**==>##核心能自由使用的頁表數量(kernel_free_pages)除以8=Kernel BitMap的長度(8位元算1單位長)=kbm_length**/

	uint32_t ubm_length = user_free_pages / 8;			// User BitMap的长度.
/**==>##使用者能自由使用的頁表數量(kernel_free_pages)除以8=User BitMap的長度(8位元算1單位長)=kbm_length**/
	//###每8個(位元組)分頁算一個單位長度，所以要除以8。
	//###若最後一組頁表不足8個，則"不使用(捨棄)"這不足8個的頁表，好處是這樣"使用的"會小於"能使用的"

//----------------------------------------------------- 
    uint32_t kp_start = used_mem;				  		// Kernel Pool start,内核内存池的起始地址
/**==>##used_mem為"被使用的總記憶體大小"(被低端1MB、分頁目錄表、頁表使用)，單位為byte，内核内存池的起始地址(kp_start)由此開始算**/
	//###頁表是從0x100000開始算的，解釋在上面
    
	uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;	  // User Pool start,用户内存池的起始地址
/**==>##内核内存池的起始地址(kp_start)+核心能自由使用的頁表數量*1個頁表的大小=使用者内存池的起始地址(up_start)**/
	//###kernel_free_pages的單位是頁表


//==========================================================================================
    kernel_pool.phy_addr_start = kp_start;
    user_pool.phy_addr_start   = up_start;
	//###kp_start在低地址處，up_start在高地址處

//------------------------------------------------------ 
    kernel_pool.pool_size = kernel_free_pages * PG_SIZE;
/**==>##kernel_pool.pool_size存的是"核心能自由使用的頁表數量(kernel_free_pages)*頁表大小"=核心能自由使用的總大小(單位為byte)**/
    
	user_pool.pool_size	 = user_free_pages * PG_SIZE;
/**==>##user_pool.pool_size存的是"使用者能自由使用的頁表數量(user_free_pages)*頁表大小"=使用者能自由使用的總大小(單位為byte)**/
 
//------------------------------------------------------ 
	kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
/**==>##kernel_pool.pool_bitmap.btmp_bytes_len存的是Kernel BitMap的長度(8位元算1單位長)**/

    user_pool.pool_bitmap.btmp_bytes_len = ubm_length;
/**==>##user_pool.pool_bitmap.btmp_bytes_len存的是User BitMap的長度(8位元算1單位長)**/
	//###struct pool裡面包struct bitmap

//------------------------------------------------------ 
    /*********    内核内存池和用户内存池位图   ***********
    *   位图是全局的数据，长度不固定。
    *   全局或静态的数组需要在编译时知道其长度，
    *   而我们需要根据总内存大小算出需要多少字节。
    *   所以改为指定一块内存来生成位图.
    *   ************************************************/
    // 内核使用的最高地址是0xc009f000,这是主线程的栈地址.(内核的大小预计为70K左右)
    // 32M内存占用的位图是2k.内核内存池的位图先定在MEM_BITMAP_BASE(0xc009a000)处.
	//	#~~~~~~~~~~~~~~~~~~^ 1k?
	/*	###選0xc009a000的原因是kernel堆疊從0xc00f000，而PCB從0xc00e000，
		###0xc00e000-0xc009a000=0x4000=4分頁大小，
		###而32MB除以一個分頁的大小(=4k)再除以8=1k=總共只需要的點陣圖大小(佔一個分頁的四分之一)，	
		###目前分配4分頁的大小給點陣圖表示可支援的總記憶體大小可以擴到32MB*4*4=512MB		*/ 
	kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;
/**==>##kernel_pool.pool_bitmap.bits存的是核心的點陣圖的堆疊的起始地址**/

    /* 用户内存池的位图紧跟在内核内存池位图之后 */
    user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);
/**==>##user_pool.pool_bitmap.bits存的是核心的點陣圖的堆疊的起始地址+Kernel BitMap的長度(8位元算1單位長)=使用者的點陣圖的堆疊的起始地址**/


//==========================================================================================
    /******************** 输出内存池信息 **********************/
    put_str("      kernel_pool_bitmap_start:");
	put_int((int)kernel_pool.pool_bitmap.bits);
    put_str("\n");
	
    put_str("      kernel_pool_bitmap_end:");
    put_int((int)kernel_pool.pool_bitmap.bits + kernel_pool.pool_bitmap.btmp_bytes_len);
    put_str("\n");
	
    put_str("       kernel_pool_phy_addr_start:");put_int(kernel_pool.phy_addr_start);
    put_str("\n");
	
    put_str("       kernel_pool_phy_addr_end:");
    put_int(kernel_pool.phy_addr_start + kernel_pool.pool_size);
    put_str("\n");
	
	//----------------------------------------
    put_str("      user_pool_bitmap_start:");
    put_int((int)user_pool.pool_bitmap.bits);
    put_str("\n");
	
    put_str("      user_pool_bitmap_end:");
    put_int((int)user_pool.pool_bitmap.bits + user_pool.pool_bitmap.btmp_bytes_len);
    put_str("\n");
	
    put_str("       user_pool_phy_addr_start:");
    put_int(user_pool.phy_addr_start);
    put_str("\n");
	
    put_str("       user_pool_phy_addr_end:");
    put_int(user_pool.phy_addr_start + user_pool.pool_size);
    put_str("\n");

    /* 将位图置0*/
    bitmap_init(&kernel_pool.pool_bitmap);
    bitmap_init(&user_pool.pool_bitmap);

//~~~~~~~~~~~~~~~~~~~~~~~~~第11章b~~~~~~~~~~~~~~~~~~~~~~~~~~~~	
	lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);
/*
###需要注意:
###	lock_init函數內容在sync.c，
###	此函數內含有sema_init函數，
### sema_init函數除了初始化value，還會呼叫list_init，
###	把struct   list waiters 也初始化。	*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//==========================================================================================
	/* 下面初始化内核虚拟地址的位图,按实际物理内存大小生成数组。*/
	// 用于维护内核堆的虚拟地址,所以要和内核内存池大小一致
	kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;
/**==>##kernel_vaddr.vaddr_bitmap.btmp_bytes_len存的內容=Kernel BitMap的長度(8位元算1單位長)**/

    /* 位图的数组指向一块未使用的内存,目前定位在内核内存池和用户内存池之外*/
    kernel_vaddr.vaddr_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length + ubm_length);
/**==>##kernel_vaddr.vaddr_bitmap.bits存的是未被核心和使用者使用的點陣圖的起始位址，即虛擬位址的點陣圖的器始位址**/
    
	kernel_vaddr.vaddr_start = K_HEAP_START;
/**==>##kernel_vaddr.vaddr_bitmap.bits存的是跨過低端1MB的起始地址**/
/*	###K_HEAP_START=0xc0100000=1100000000_0100000000_000000000000
	###="分頁目錄取c00、分頁表取400、偏移量取0"(筆記照相的記憶體圖的紅色粗箭頭位置)	*/


//==========================================================================================	
    put_str("     kernel_vaddr.vaddr_bitmap.start:");
    put_int((int)kernel_vaddr.vaddr_bitmap.bits);
    put_str("\n");
	
    put_str("     kernel_vaddr.vaddr_bitmap.end:");
    put_int((int)kernel_vaddr.vaddr_bitmap.bits + kernel_vaddr.vaddr_bitmap.btmp_bytes_len);
    put_str("\n");

    bitmap_init(&kernel_vaddr.vaddr_bitmap);
    put_str("   mem_pool_init done\n");
	
/*	###統整:
	###	kp_start和up_start都在0x200000(實體)以上的位置(看筆記照相的記憶體圖!)
	###	K_HEAP_START=虛擬起始位=0xc0100000(虛擬)=筆記照相圖的紅色粗箭頭處	*/
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第12章e~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 返回arena中第idx个内存块的地址 */
static struct mem_block* arena2block(struct arena* a, uint32_t idx) {
    return (struct mem_block*)((uint32_t)a + sizeof(struct arena) + idx * a->desc->block_size);
}

/* 返回内存块b所在的arena地址 */
static struct arena* block2arena(struct mem_block* b) {
    return (struct arena*)((uint32_t)b & 0xfffff000);
}

/* 在堆中申请size字节内存 */
void* sys_malloc(uint32_t size) {
   enum pool_flags PF;
   struct pool* mem_pool;
   uint32_t pool_size;
   struct mem_block_desc* descs;
   struct task_struct* cur_thread = running_thread();

/* 判断用哪个内存池*/
   if (cur_thread->pgdir == NULL) {     // 若为内核线程
      PF = PF_KERNEL; 
      pool_size = kernel_pool.pool_size;
      mem_pool = &kernel_pool;
      descs = k_block_descs;
   } 
   else {				      // 用户进程pcb中的pgdir会在为其分配页表时创建
      PF = PF_USER;
      pool_size = user_pool.pool_size;
      mem_pool = &user_pool;
      descs = cur_thread->u_block_desc;
   }

   /* 若申请的内存不在内存池容量范围内则直接返回NULL */
   if (!(size > 0 && size < pool_size)) {
      return NULL;
   }
   struct arena* a;
   struct mem_block* b;	
   lock_acquire(&mem_pool->lock);

/* 超过最大内存块1024, 就分配页框 */
   if (size > 1024) {
      uint32_t page_cnt = DIV_ROUND_UP(size + sizeof(struct arena), PG_SIZE);    // 向上取整需要的页框数

      a = malloc_page(PF, page_cnt);

      if (a != NULL) {
		memset(a, 0, page_cnt * PG_SIZE);	 // 将分配的内存清0  

		/* 对于分配的大块页框,将desc置为NULL, cnt置为页框数,large置为true */
		a->desc = NULL;
		a->cnt = page_cnt;
		a->large = true;
		lock_release(&mem_pool->lock);
		
		return (void*)(a + 1);		 // 跨过arena大小，把剩下的内存返回
	/*	###int a[x]; a+1實際是橫跨4位元組的大小，因為int佔4位元組，
		###因此a + 1就是橫跨1個struct arena* a之結構的大小。	*/
      } 
	  else { 
		lock_release(&mem_pool->lock);
		return NULL; 
      }
   }   
   
   else {    // 若申请的内存小于等于1024,可在各种规格的mem_block_desc中去适配
      uint8_t desc_idx;
      
      /* 从内存块描述符中匹配合适的内存块规格 */
      for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
		if (size <= descs[desc_idx].block_size) {  // 从小往大后,找到后退出
			break;
		}
      }

   /* 若mem_block_desc的free_list中已经没有可用的mem_block,
    * 就创建新的arena提供mem_block */
      if (list_empty(&descs[desc_idx].free_list)) {
		a = malloc_page(PF, 1);       // 分配1页框做为arena
		
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第15章g~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~	
		struct task_struct* cur_thread = running_thread();//<=================================
		page_dir_activate(cur_thread);                    //<=================================
		/*
		####bochs會出現bug!!!
		####不知為何，bochs會誤讀分頁表，把虛擬地址誤映射到物理位址
		####也不知為何原因，重新載入cr3可以消除此bug!!!
		*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		
		if (a == NULL) {
			lock_release(&mem_pool->lock);
			return NULL;
		}
		memset(a, 0, PG_SIZE);
	
		/* 对于分配的小块内存,将desc置为相应内存块描述符, 
		* cnt置为此arena可用的内存块数,large置为false */
		a->desc = &descs[desc_idx];
		a->large = false;
		a->cnt = descs[desc_idx].blocks_per_arena;
		uint32_t block_idx;
	
		enum intr_status old_status = intr_disable();
	
		/* 开始将arena拆分成内存块,并添加到内存块描述符的free_list中 */
		for (block_idx = 0; block_idx < descs[desc_idx].blocks_per_arena; block_idx++) {
			b = arena2block(a, block_idx);
			ASSERT(!elem_find(&a->desc->free_list, &b->free_elem));
			list_append(&a->desc->free_list, &b->free_elem);	
		}
		intr_set_status(old_status);
      }    

   /* 开始分配内存块 */
      b = elem2entry(struct mem_block, free_elem, list_pop(&(descs[desc_idx].free_list)));
      memset(b, 0, descs[desc_idx].block_size);

      a = block2arena(b);  // 获取内存块b所在的arena
      a->cnt--;		   // 将此arena中的空闲内存块数减1
      lock_release(&mem_pool->lock);
      return (void*)b;
   }
}
/*	###統整:
	###	sys_malloc函數主要是在做筆記照相的事，先確認你要申請的記憶體的size對應到的 區塊描述符號的 free_list 有沒有串列在上面
	###	(核心只有唯一一個 區塊描述陣列 ，而每個使用者都有一個專屬的 區塊描述陣列 )，
	###	沒有的話要先申請分頁，然後把一個一個串列掛在free_list裡面，
	###	
	###	先判斷cur_thread是核心還是使用者，賦予PF、pool_size、mem_pool、descs對應的數值，
	###	再看你申請的記憶體多大，如果大於1024，則計算你要申請的大小 + struct arena的大小 佔多少分頁，
	###	餘數不足1分頁直接DIV_ROUND_UP一個分頁，然後返回跨過 arena 後的起始位址，
	###	
	###	如果你申請的記憶體小於等於1024，則要看你申請的記憶體對應到哪個 區塊描述符號，
	###	例如你現在是 malloc(66) ，66屬於16、32、64、"128"(選能塞得下66Byte的方案)，
	###	若該方案對應到的 區段描述符號的 free_list 沒有被掛上任何的 串列，
	###		則申請一個新的分頁，把arena(即a)的資訊填在新申請的分頁最底下(佔12位元組)，
	###		然後用for迴圈一一求出每個串列的 基底虛擬位址(b) 並一個一個放入 剛對應到的 區段描述符號的 free_list 內，
	###	若該方案對應到的 區段描述符號的 free_list 還有 串列 掛在上面，則跳過以上被縮排的動作，
	###	然後從 free_list中 pop出一個 串列 ，用 elem2entry巨集 得到該串列的基底位址(b)，
	###	接著用memset函數把 剛pop出的串列的 prev和next 洗掉，
	###	然後把arena(即a)的cnt-1(好像沒有這個cnt也沒差? 因為用list_empty函數就知道free_list是不是空的了)，
	###	最後返回 剛pop出的串列的 基底位址(b) 。	
	###	以上請看筆記照相對照理解!!!																					*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第12章f~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 将物理地址pg_phy_addr回收到物理内存池 */
void pfree(uint32_t pg_phy_addr) {
   struct pool* mem_pool;
   uint32_t bit_idx = 0;
   if (pg_phy_addr >= user_pool.phy_addr_start) {     // 用户物理内存池
      mem_pool = &user_pool;
      bit_idx = (pg_phy_addr - user_pool.phy_addr_start) / PG_SIZE;
   } 
   else {	  // 内核物理内存池
      mem_pool = &kernel_pool;
      bit_idx = (pg_phy_addr - kernel_pool.phy_addr_start) / PG_SIZE;
   }
   bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);	 // 将位图中该位清0
}

/* 去掉页表中虚拟地址vaddr的映射,只去掉vaddr对应的pte */
static void page_table_pte_remove(uint32_t vaddr) {
   uint32_t* pte = pte_ptr(vaddr);
   *pte &= ~PG_P_1;	// 将页表项pte的P位置0
   asm volatile ("invlpg %0"::"m" (vaddr):"memory");    //更新tlb
}

/* 在虚拟地址池中释放以_vaddr起始的连续pg_cnt个虚拟页地址 */
static void vaddr_remove(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt) {
   uint32_t bit_idx_start = 0, vaddr = (uint32_t)_vaddr, cnt = 0;

   if (pf == PF_KERNEL) {  // 内核虚拟内存池
      bit_idx_start = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
      while(cnt < pg_cnt) {
		bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
      }
   } 
   else {  // 用户虚拟内存池
      struct task_struct* cur_thread = running_thread();
      bit_idx_start = (vaddr - cur_thread->userprog_vaddr.vaddr_start) / PG_SIZE;
      while(cnt < pg_cnt) {
		bitmap_set(&cur_thread->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
      }
   }
}

/* 释放以虚拟地址vaddr为起始的cnt个物理页框 */
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt) {
	uint32_t pg_phy_addr;
	uint32_t vaddr = (int32_t)_vaddr, page_cnt = 0;
	ASSERT(pg_cnt >=1 && vaddr % PG_SIZE == 0); 
	pg_phy_addr = addr_v2p(vaddr);  // 获取虚拟地址vaddr对应的物理地址

	/* 确保待释放的物理内存在低端1M+1k大小的页目录+1k大小的页表地址范围外 */
	//								^~~~~~		   ^~~~~~	4k大小?
	ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= 0x102000);
   
	/* 判断pg_phy_addr属于用户物理内存池还是内核物理内存池 */
	if (pg_phy_addr >= user_pool.phy_addr_start) {   // 位于user_pool内存池
		vaddr -= PG_SIZE;
		while (page_cnt < pg_cnt) {
			vaddr += PG_SIZE;
			pg_phy_addr = addr_v2p(vaddr);
	
			/* 确保物理地址属于用户物理内存池 */
			ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= user_pool.phy_addr_start);
	
			/* 先将对应的物理页框归还到内存池 */
			pfree(pg_phy_addr);
	
			/* 再从页表中清除此虚拟地址所在的页表项pte */
			page_table_pte_remove(vaddr);
	
			page_cnt++;
		}
		/* 清空虚拟地址的位图中的相应位 */
		vaddr_remove(pf, _vaddr, pg_cnt);
	} 
	else {	     // 位于kernel_pool内存池
		vaddr -= PG_SIZE;	      
		while (page_cnt < pg_cnt) {
			vaddr += PG_SIZE;
			pg_phy_addr = addr_v2p(vaddr);
			/* 确保待释放的物理内存只属于内核物理内存池 */
			ASSERT((pg_phy_addr % PG_SIZE) == 0 && \
			pg_phy_addr >= kernel_pool.phy_addr_start && \
			pg_phy_addr < user_pool.phy_addr_start);
		
			/* 先将对应的物理页框归还到内存池 */
			pfree(pg_phy_addr);
		
			/* 再从页表中清除此虚拟地址所在的页表项pte */
			page_table_pte_remove(vaddr);
		
			page_cnt++;
		}
	/*	也可以寫成這樣?      
		while (pg_cnt-- > 0) {
			pg_phy_addr = addr_v2p(vaddr);
			确保待释放的物理内存只属于内核物理内存池
			ASSERT((pg_phy_addr % PG_SIZE) == 0 && \
			pg_phy_addr >= kernel_pool.phy_addr_start && \
			pg_phy_addr < user_pool.phy_addr_start);
		
			先将对应的物理页框归还到内存池
			pfree(pg_phy_addr);
		
			再从页表中清除此虚拟地址所在的页表项pte
			page_table_pte_remove(vaddr);
		
			vaddr += PG_SIZE;
		}	*/

   	/* 清空虚拟地址的位图中的相应位 */
   	vaddr_remove(pf, _vaddr, pg_cnt);
	}
}
/*	###統整:
	###	pfree函數作用跟palloc相反，目的在移除一個物理地址，方法為把 物理地址 對應到的 物理地址點陣圖位置 歸0，
	###	
	###	
	###	page_table_pte_remov函數e為page_table_add的相反，目的是把 虛擬為址 對應到表的位置 清空，
	###	方法為把存黑色箭頭位置的P位清0就可以了，這樣CPU就會覺得黑色箭頭已經不存在了，
	###	CPU還會把用過的虛擬為址填入自己的TLB內，所以要也要清空TLB內 該虛擬位址 的內容，用invlpg指令即可，
	###	
	###	
	###	vaddr_remove函數為vaddr_get的相反，目的在釋放連續pg_cnt個用過的 虛擬位址，
	###	方法為用while迴圈把 虛擬位址點陣圖 對應到的 pg_cnt個位元清0，									
	###
	###
	###	mfree_page函數為malloc_page的相反，目的在釋放 以傳入的虛擬地址為起始的 pg_cnt個分頁，
	###	方法為用addr_v2p函數先把把傳入的 虛擬地址 轉換成 物理地址，
	###	然後判斷這個 物理地址 是在 核心池 還是 使用者池 來推測 欲釋放記憶體者是 核心 還是 使用者，
	###	然後一一用pfree函數把對應到的物理地址移除，並用page_table_pte_remove函數把 虛擬為址 對應到表的位置 清空，
	###	最後用vaddr_remove函數 釋放連續pg_cnt個用過的 虛擬位址。	*/ 
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第12章g~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 回收内存ptr */
void sys_free(void* ptr) {
   ASSERT(ptr != NULL);
   if (ptr != NULL) {
      enum pool_flags PF;
      struct pool* mem_pool;

	/* 判断是线程还是进程 */
      if (running_thread()->pgdir == NULL) {
		ASSERT((uint32_t)ptr >= K_HEAP_START);
		PF = PF_KERNEL; 
		mem_pool = &kernel_pool;
      } 
	  else {
		PF = PF_USER;
		mem_pool = &user_pool;
      }

      lock_acquire(&mem_pool->lock);   
      struct mem_block* b = ptr;
      struct arena* a = block2arena(b);	     // 把mem_block转换成arena,获取元信息
      ASSERT(a->large == 0 || a->large == 1);
      if (a->desc == NULL && a->large == true) { // 大于1024的内存
		mfree_page(PF, a, a->cnt); 
      } 
	  else {				 // 小于等于1024的内存块
		/* 先将内存块回收到free_list */
		list_append(&a->desc->free_list, &b->free_elem);

		/* 再判断此arena中的内存块是否都是空闲,如果是就释放arena */
		if (++a->cnt == a->desc->blocks_per_arena) {
			uint32_t block_idx;
			for (block_idx = 0; block_idx < a->desc->blocks_per_arena; block_idx++) {
				struct mem_block*  b = arena2block(a, block_idx);
				ASSERT(elem_find(&a->desc->free_list, &b->free_elem));
				list_remove(&b->free_elem);
			}
	    mfree_page(PF, a, 1); 
		} 
      }   
      lock_release(&mem_pool->lock); 
   }
}

/*	###統整:
	###	sys_free函數為sys_malloc的反向，目的是把用完的 串列 重新塞回 free_list中，
	###	若arena所屬的所有串列(b)全部都塞回去了，那要把arena所屬的分頁整個釋放掉，
	###	方法為先判斷欲釋放的虛擬地址是屬於使用者還是核心，然後用虛擬地址回推b為何，
	###	再用b回推arena(a)為何，然後用a回推要釋放的是大記憶體還是小記憶體，
	###	
	###	如果要釋放的是大記憶體(大於1024Byte)，則直接把arena所屬的所有分頁釋放掉即可(b只剩回推a用)，
	###	
	###	如果要釋放的是小記憶體(小於等於1024Byte)，則把b塞回free_list，然後a->cnt加1，
	###	此時若arena所屬的所有串列(b)都塞回去了，即a->cnt == a->desc->blocks_per_arena，則要把arena所屬的分頁整個釋放掉，
	###	用for迴圈把b一個一個從free_list remove掉，最後再釋放掉arena所屬的分頁，
	###	請看筆記照相!!!															*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

/* 内存管理部分初始化入口 */
void mem_init() {
    put_str("mem_init start\n");
    uint32_t mem_bytes_total = (*(uint32_t*)(0xb00));
	//###0xb00存的是在loader.S查出來的記憶體大小，把0xb00強制轉換成指標在取指標內的內容
	
    mem_pool_init(mem_bytes_total);	  // 初始化内存池
	
	//~~~~~~~~~~~~~~~~~~~~~第12章e~~~~~~~~~~~~~~~~~~~~~~~
	/* 初始化mem_block_desc数组descs,为malloc做准备 */
    block_desc_init(k_block_descs);
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	
    put_str("mem_init done\n");
}

/*
###需要注意:
### 當使用者系統呼叫了以後，會從TSS拿核心態的esp，
### 因此running_thread函數傳回來的就會是使用者的task_struct的基底位址。
###
### 在第15章g修改了一些memory.c內的sys_malloc避掉bochs虛擬位址映射到實體位址的bug，
### 同時把作者寫的get_a_page函數內的ASSERT(bit_idx > 0)修改成ASSERT(bit_idx > 0)。
*/
