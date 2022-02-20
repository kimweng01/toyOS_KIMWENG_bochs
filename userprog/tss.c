#include "tss.h"
#include "stdint.h"
#include "global.h"
#include "string.h"
#include "print.h"

/* 任务状态段tss结构 */
struct tss {
    uint32_t backlink;
    uint32_t* esp0;
    uint32_t ss0;
    uint32_t* esp1;
    uint32_t ss1;
    uint32_t* esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t (*eip) (void);
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint32_t trace;
    uint32_t io_base;
}; 
static struct tss tss;

/* 更新tss中esp0字段的值为pthread的0级线 */
void update_tss_esp(struct task_struct* pthread) {
   tss.esp0 = (uint32_t*)((uint32_t)pthread + PG_SIZE);
}

/* 创建gdt描述符 */
static struct gdt_desc make_gdt_desc(uint32_t* desc_addr, uint32_t limit, uint8_t attr_low, uint8_t attr_high) {
   uint32_t desc_base = (uint32_t)desc_addr;											
   struct gdt_desc desc;
   
//----------------低32位--------------------
   desc.limit_low_word = limit & 0x0000ffff;											//##段界線(15~0)
   desc.base_low_word = desc_base & 0x0000ffff;											//##段基址(15~0)
   
//----------------高32位--------------------
   desc.base_mid_byte = ((desc_base & 0x00ff0000) >> 16);								//##段基址(23~16)
   desc.attr_low_byte = (uint8_t)(attr_low);											//##P~TYPE
   desc.limit_high_attr_high = (((limit & 0x000f0000) >> 16) + (uint8_t)(attr_high)); 	//##段界線(19~16)+G~AVL
   desc.base_high_byte = desc_base >> 24;												//##段基址(31~24)
   
   return desc;
}

/* 在gdt中创建tss并重新加载gdt */
void tss_init() {
   put_str("tss_init start\n");
   uint32_t tss_size = sizeof(tss);
   memset(&tss, 0, tss_size);
   tss.ss0 = SELECTOR_K_STACK;
   tss.io_base = tss_size;

/* gdt段基址为0x900,把tss放到第4个位置,也就是0x900+0x20的位置 */

  /* 在gdt中添加dpl为0的TSS描述符 */
  *((struct gdt_desc*)0xc0000920) = make_gdt_desc((uint32_t*)&tss, tss_size - 1, TSS_ATTR_LOW, TSS_ATTR_HIGH);

  /* 在gdt中添加dpl为3的数据段和代码段描述符 */
  *((struct gdt_desc*)0xc0000928) = make_gdt_desc((uint32_t*)0, 0xfffff, GDT_CODE_ATTR_LOW_DPL3, GDT_ATTR_HIGH);
  *((struct gdt_desc*)0xc0000930) = make_gdt_desc((uint32_t*)0, 0xfffff, GDT_DATA_ATTR_LOW_DPL3, GDT_ATTR_HIGH);
/*	###											  ~~~~~~~~~~~^  ~~~~~~^  ~~~~~~~~~~~~~~~~~~~~~^  ~~~~~~~~~~~~^
	###												 段基址		 段界線			 P~TYPE				  G~AVL			
	###																					定義在global.h中		*/
 
  /* gdt 16位的limit 32位的段基址 */
   uint64_t gdt_operand = ((8 * 7 - 1) | ((uint64_t)(uint32_t)0xc0000900 << 16));   // 7个描述符大小
   //###包括第0個總共有7個段描述符號，佔了8 * 7 - 1個位元組(從0開始算)，此值為GDT界線。
   
   asm volatile ("lgdt %0" : : "m" (gdt_operand));
   asm volatile ("ltr %w0" : : "r" (SELECTOR_TSS)); //##TR只佔16位元
   
   put_str("tss_init and ltr done\n");
}

/*
###需要注意:
###	TR內放GDT選擇子，指向TSS描述符號存於GDT的位置，
###
###	原始x86的設計是在切換工作時，從TSS描述符號內的數值進行一系列的檢查，
###	檢查完後，把暫存器的數值存入TSS描述符號指向的TSS中，
###	把新的選擇子載入TR，又進行一系列的檢查後，把新的選擇子指向的 TSS描述符號指向的 TSS內的 暫存器數值 載入 CPU的暫存器，
###	再進行一系列檢查，
###	然後把上一個TSS選擇子存於TSS的 "上一個TSS工作指標中"，
###
###	然而，因為要頻繁載入TR，所以上述方法被Linux棄用，
###	Linux的方法為(也是本code的用法):
###	TSS功能只剩下存使用者於特權等級為0時的堆疊底位置，該值存於TSS中的SS0和esp0中，
### TSS只有唯一一個，執行緒在交棒時，會進入 page_dir_activate函數 更新TSS的SS0和esp0，
###	x86 CPU有一個原生的特性，當 "目前的"工作 要從低特權到高特權時，
###	會自動拿目前 TR內的選擇子指向的 TSS描述符號指向的 TSS內的 SS0和esp0 作為0特權等級的堆疊，
###	我們在page_dir_activate函數內設置的SS0是0，所以0特權等級的堆疊位址為0+esp0=esp0，
###	然後直接拿esp0的位置(在page_dir_activate函數內設置為 目前執行緒的PCB的 頂端)來push一系列的暫存器數值進去，
*/