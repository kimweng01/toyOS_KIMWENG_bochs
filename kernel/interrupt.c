#include "interrupt.h"
#include "stdint.h"
#include "global.h"
#include "io.h"
#include "print.h"

#define PIC_M_CTRL 0x20	  //##主片:ICW1、OCW2、OCW3，这里用的可编程中断控制器是8259A,主片的控制端口是0x20
#define PIC_M_DATA 0x21	  //##主片:ICW2~ICW4、OCW1，主片的数据端口是0x21
#define PIC_S_CTRL 0xa0	  //##從片:ICW1、OCW2、OCW3，从片的控制端口是0xa0
#define PIC_S_DATA 0xa1	  //##從片:ICW2~ICW4、OCW1，从片的数据端口是0xa1
/*	###由於8259A嚴格規定端口的寫入順序，
	###例如用0x21寫入ICW1後，下一個用0x21寫入的一定是ICW2，
	###所以不會有因為端口名字相同而衝突的問題	*/

//#define IDT_DESC_CNT 0x21 // 目前总共支持的中断数
//#define IDT_DESC_CNT 0x30	//##在第10章c後，支援中斷數添加到0x30個
#define IDT_DESC_CNT 0x81   //##在第十二章a後，支援中斷數添加到0x81個

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第8章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
#define EFLAGS_IF   0x00000200       // eflags寄存器中的if位为1
#define GET_EFLAGS(EFLAG_VAR) asm volatile("pushfl; popl %0" : "=g" (EFLAG_VAR))
/*	###把"EFLAGS暫存器"壓入棧，再pop 32位元到gs，再從gs傳入EFLAG_VAR，
	###EFLAG_VAR會變成呼叫GET_EFLAGS(eflags)的eflags	*/
	
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
extern uint32_t syscall_handler(void);


//====================================================================================================
/*中断门描述符结构体*/
struct gate_desc {
    uint16_t    func_offset_low_word;
    uint16_t    selector;
    uint8_t     dcount;   //此项为双字计数字段，是门描述符中的第4字节。此项固定值，不用考虑
	//##dcount為中斷門描述符未使用的部分
	
    uint8_t     attribute;
    uint16_t    func_offset_high_word;
};

// 静态函数声明,非必须
static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr, intr_handler function);
															 // ###^~~~~~~~~~~~ typedef void* intr_handler，定義在interrupt.h。
static struct gate_desc idt[IDT_DESC_CNT];// ###idt是中断描述符表,本质上就是个中断门描述符数组
//			 			^~~~~~~~~~~~~~~~~
/*	###定義一個陣列，共有IDT_DESC_CNT個元素，裡面放的都是中斷門描述符號(struct gate_desc)，
	###這個陣列第0個struct gate_desc的位址是idt。	*/

//--------------------------------------------------------------
char* intr_name[IDT_DESC_CNT];		      // 用于保存异常的名字
intr_handler idt_table[IDT_DESC_CNT];	  // 定义中断处理程序数组.在kernel.S中定义的intrXXentry只是中断处理程序的入口,最终调用的是ide_table中的处理程序
//			 ^~~~~~~~~~~~~~~~~~~~~~~
extern intr_handler intr_entry_table[IDT_DESC_CNT];	    // ###声明引用定义在kernel.S中的中断处理函数入口数组
/*	###intr_handler為void*，定義在interrupt.h，
	###void* 表示此類型是一個指標，指向任何類型皆可，
	###intr_entry_table已經定義在kernel.S中，但未定義型別，
	###所以在此用extern把intr_entry_table引用出來定義型別為intr_handler。	*/

/*
#################################################################
##整理:                                                        ##
##	idt[IDT_DESC_CNT]裡面放的是 中斷門描述符號 ，共0x21個。    ##
##	idt_table[IDT_DESC_CNT]裡面放的是 中斷處理常式 ，共0x21個。##
#################################################################
*/                                                             

//====================================================================================================
/* 初始化可编程中断控制器8259A */
static void pic_init(void) {

    /* 初始化主片 */
    outb (PIC_M_CTRL, 0x11);   // ICW1: 边沿触发,级联8259, 需要ICW4.
    outb (PIC_M_DATA, 0x20);   // ICW2: 起始中断向量号为0x20,也就是IR[0-7] 为 0x20 ~ 0x27.
    outb (PIC_M_DATA, 0x04);   // ICW3: IR2接从片.
/*	###主片像插座，從片像延長線，
	###延長線的插頭插入主片的IR2，即為"串聯"。	*/
    outb (PIC_M_DATA, 0x01);   // ICW4: 8086模式, 正常EOI

    /* 初始化从片 */
    outb (PIC_S_CTRL, 0x11);	// ICW1: 边沿触发,级联8259, 需要ICW4.
    outb (PIC_S_DATA, 0x28);	// ICW2: 起始中断向量号为0x28,也就是IR[8-15] 为 0x28 ~ 0x2F.
    outb (PIC_S_DATA, 0x02);	// ICW3: 设置从片连接到主片的IR2引脚
    outb (PIC_S_DATA, 0x01);	// ICW4: 8086模式, 正常EOI
/*	###上面藉由outb (PIC_M_DATA, 0x20)可知起始中斷向量為0x20=32，
	###32開始為Maskable Interrupts的範圍，IR0為時脈中斷，在此設置只打開此中斷	*/

/* IRQ2用于级联从片,必须打开,否则无法响应从片上的中断
  主片上打开的中断有IRQ0的时钟,IRQ1的键盘和级联从片的IRQ2,其它全部关闭 */
   outb (PIC_M_DATA, 0xf8);

/* 打开从片上的IRQ14,此引脚接收硬盘控制器的中断 */
   outb (PIC_S_DATA, 0xbf);
  
/*	###8259A類似MIPS32的CP0，控制CPU的中斷訊號，
	###比較不一樣的是，8259A的方式是通知CPU要中斷，然後CPU"自己"去藉由IDTR查IDT的位置，
	###然後藉由中斷號找IDT內對應的中斷門描述符號，內有GDT選擇子，
	###再藉由GDT選擇子找GDT內對應的段描述符號，段描述符號內有中斷處理常式的段基址，
	###再藉由中斷門描述符號內的中斷處理常式偏移量加上中斷處理常式的段基址，
	###得到中斷處理常式的最終位址。
	###而CP0是"直接給"CPU中斷處理常式位址，CPU不用花大量功夫處理	*/

    put_str("   pic_init done\n");
}

/* 创建中断门描述符 */
static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr, intr_handler function) { 
    p_gdesc->func_offset_low_word = (uint32_t)function & 0x0000FFFF;
	//##intr_entry_table[i]傳入make_idt_desc變成function
/*	###kernel的起始位置在0xc0001500，
	###intr_entry_table為中斷處理常式"群(表)"所在的位址，
	###intr_entry_table[i]為"群中"的第i個中斷處理常式在記憶體的位址，即0xc000XXXX，
	###由於在平坦模式下，段基址為0，所以0xc000XXXX可以直接拿來當偏移量使用。	*/

    p_gdesc->selector = SELECTOR_K_CODE;
    p_gdesc->dcount = 0;
	//##中斷門描述符未使用的部分
	
    p_gdesc->attribute = attr;
//	###以IDT_DESC_ATTR_DPL0傳入，其內容定義在global.h。
	
    p_gdesc->func_offset_high_word = ((uint32_t)function & 0xFFFF0000) >> 16;
}

/*
//~~~~~~~~~~~~~~~~~~~~~~~~~第12章以前用的~~~~~~~~~~~~~~~~~~~~~~~~~~~
初始化中断描述符表
static void idt_desc_init(void) {
    int i;

    for (i = 0; i < IDT_DESC_CNT; i++) {
        make_idt_desc(&idt[i], IDT_DESC_ATTR_DPL0, intr_entry_table[i]);
			###idt[i]表示IDT中第i個中斷門描述符號(struct gate_desc)，
			###&idt[i]代表第i個中斷門描述符號(struct gate_desc)的位址，
		 	###&idt[i]傳入make_idt_desc會變成p_gdesc，
			###藉由p_gdesc->XXX為中斷門描述符號(struct gate_desc)的內容賦值。
    }
    put_str("   idt_desc_init done\n");
}
*/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~第12章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*初始化中断描述符表*/
static void idt_desc_init(void) {
   int i, lastindex = IDT_DESC_CNT - 1; //##lastindex = 0x80
  
   for (i = 0; i < IDT_DESC_CNT; i++) {
      make_idt_desc(&idt[i], IDT_DESC_ATTR_DPL0, intr_entry_table[i]); 
/*	###idt[i]表示IDT中第i個中斷門描述符號(struct gate_desc)，
	###&idt[i]代表第i個中斷門描述符號(struct gate_desc)的位址，
 	###&idt[i]傳入make_idt_desc會變成p_gdesc，
	###藉由p_gdesc->XXX為中斷門描述符號(struct gate_desc)的內容賦值。	*/
   }

/* 单独处理系统调用,系统调用对应的中断门dpl为3,
 * 中断处理程序为单独的syscall_handler */
   make_idt_desc(&idt[lastindex], IDT_DESC_ATTR_DPL3, syscall_handler);
   
   put_str("   idt_desc_init done\n");
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第7章b~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 通用的中断处理函数,一般用在异常出现时的处理 */
static void general_intr_handler(uint8_t vec_nr) {
   if (vec_nr == 0x27 || vec_nr == 0x2f || vec_nr == 0x0d) {		// 0x2f是从片8259A上的最后一个irq引脚，保留
      return;		//##IRQ7和IRQ15会产生伪中断(spurious interrupt),无须处理。
   }
   
   /*
   ###第9章c以前採用此中斷處理常式
   put_str("int vector: 0x");
   put_int(vec_nr);
   put_char('\n');
   */
   
   //~~~~~~~~~~~~~~~~~~~~~~~~~第9章c~~~~~~~~~~~~~~~~~~~~~~~
   /* 将光标置为0,从屏幕左上角清出一片打印异常信息的区域,方便阅读 */
   set_cursor(0);
   int cursor_pos = 0;
   while(cursor_pos < 320) {
      put_char(' ');
      cursor_pos++;
   }

   set_cursor(0);	 // 重置光标为屏幕左上角
   put_str("!!!!!!!      excetion message begin  !!!!!!!!\n");
   set_cursor(88);	// 从第2行第8个字符开始打印
   put_str(intr_name[vec_nr]);
   if (vec_nr == 14) {	  // 若为Pagefault,将缺失的地址打印出来并悬停
      int page_fault_vaddr = 0; 
      asm ("movl %%cr2, %0" : "=r" (page_fault_vaddr));	  // cr2是存放造成page_fault的地址
      put_str("\npage fault addr is ");put_int(page_fault_vaddr); 
   }
   put_str("\n!!!!!!!      excetion message end    !!!!!!!!\n");
  // 能进入中断处理程序就表示已经处在关中断情况下,
  // 不会出现调度进程的情况。故下面的死循环不会再被中断。
   while(1);
   //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
}


/* 完成一般中断处理函数注册及异常名称注册 */
static void exception_init(void) {			    // 完成一般中断处理函数注册及异常名称注册
   int i;
   for (i = 0; i < IDT_DESC_CNT; i++) {

/* idt_table数组中的函数是在进入中断后根据中断向量号调用的,
 * 见kernel/kernel.S的call [idt_table + %1*4] */
      idt_table[i] = general_intr_handler;		// 默认为general_intr_handler。
							   // 以后会由register_handler来注册具体处理函数。
      intr_name[i] = "unknown";				    //##先统一赋值为unknown 
   }
   intr_name[0] = "#DE Divide Error";
   intr_name[1] = "#DB Debug Exception";
   intr_name[2] = "NMI Interrupt";
   intr_name[3] = "#BP Breakpoint Exception";
   intr_name[4] = "#OF Overflow Exception";
   intr_name[5] = "#BR BOUND Range Exceeded Exception";
   intr_name[6] = "#UD Invalid Opcode Exception";
   intr_name[7] = "#NM Device Not Available Exception";
   intr_name[8] = "#DF Double Fault Exception";
   intr_name[9] = "Coprocessor Segment Overrun";
   intr_name[10] = "#TS Invalid TSS Exception";
   intr_name[11] = "#NP Segment Not Present";
   intr_name[12] = "#SS Stack Fault Exception";
   intr_name[13] = "#GP General Protection Exception";
   intr_name[14] = "#PF Page-Fault Exception";
   // intr_name[15] 第15项是intel保留项，未使用
   intr_name[16] = "#MF x87 FPU Floating-Point Error";
   intr_name[17] = "#AC Alignment Check Exception";
   intr_name[18] = "#MC Machine-Check Exception";
   intr_name[19] = "#XF SIMD Floating-Point Exception";

}
/*
####################################################################################################
##需要注意:                                                                                       ##
##	在組合語言，函數的呼叫方式就是 call 函數名字，                                                ##
##	所以函數的名字本質就是個地址，    	                                                          ##
##	把這i個 中斷處理常式的地址 放入idt_table[i]中，共放0x21個，                                   ##
##	[idt_table + %1*4]為放 欲存取的 中斷處理常式的地址，                                          ##
##	call [idt_table + %1*4] 等於 call 欲存取的 中斷處理常式的地址                                 ##
####################################################################################################
*/ 

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第8章a~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 开中断并返回开中断前的状态*/
enum intr_status intr_enable() {
   enum intr_status old_status;
   if (INTR_ON == intr_get_status()) {
      old_status = INTR_ON;
      return old_status;
   } 
   else {
      old_status = INTR_OFF;
      asm volatile("sti");	 // 开中断,sti指令将IF位置1
      return old_status;
   }
}
/*	###開中斷函數，此函數會檢查現在的狀態，
	###若現在中斷是開啟的，不處理，
	###若現在中斷是關閉的，把IF位置設1，	
	###此函數要回傳處理前的狀態，例如原本的中斷是關閉的，要回傳INTR_OFF。	*/

/* 关中断,并且返回关中断前的状态 */
enum intr_status intr_disable() {     
   enum intr_status old_status;
   if (INTR_ON == intr_get_status()) {
      old_status = INTR_ON;
      asm volatile("cli" : : : "memory"); // 关中断,cli指令将IF位置0
      return old_status;
   } 
   else {
      old_status = INTR_OFF;
      return old_status;
   }
}
/*	###關中斷函數，原理跟開中斷函數大致相同	 */

/* 将中断状态设置为status */
enum intr_status intr_set_status(enum intr_status status) {
   return status & INTR_ON ? intr_enable() : intr_disable();
}

/* 获取当前中断状态 */
enum intr_status intr_get_status() {
   uint32_t eflags = 0;
   GET_EFLAGS(eflags);
/*	###GET_EFLAGS是巨集函數，用巨集的方式定義，
	###eflags會變成在巨集函數內的EFLAG_VAR的最終值	*/
   return (EFLAGS_IF & eflags) ? INTR_ON : INTR_OFF;
/*	###判斷eflags的IF位元是否為1，是的話INTR_ON	*/
}
/*	###此函數的回傳值是列舉結構，定義在interrupt.h中，列舉中只有兩個元素，
	###即INTR_OFF和INTR_ON，所以此函數回傳值只會有INTR_OFF和INTR_ON兩種		*/
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~第9章c~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* 在中断处理程序数组第vector_no个元素中注册安装中断处理程序function */
void register_handler(uint8_t vector_no, intr_handler function) {
/* idt_table数组中的函数是在进入中断后根据中断向量号调用的,
 * 见kernel/kernel.S的call [idt_table + %1*4] */
   idt_table[vector_no] = function; 
}
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

/*完成有关中断的所有初始化工作*/
void idt_init() {
    put_str("idt_init start\n");
    idt_desc_init();	   // ###初始化 中断描述符表
	exception_init();	   // ###异常名初始化并注册通常的 中断处理函数
    pic_init();		   	   // 初始化8259A

    /* 加载idt */
    uint64_t idt_operand = ( (uint64_t)(uint32_t)idt << 16 | (sizeof(idt) - 1) );
	/*	###此為要放入中斷描述符號暫存器(IDTR)的內容，高32位存IDT的基礎位址，
		###其值為idt，低16位元為IDT的表界線，其值為sizeof(idt)-1，從0開始算。	*/
	
    asm volatile("lidt %0" : : "m" (idt_operand));
	//	###把中斷描述符號暫存器的內容放入CPU內的中斷描述符號暫存器(IDTR)
	
    put_str("idt_init done\n");
}

/*
###IDT存於GDT中，IDT暫存器(IDTR)指示IDT的基底位址，
###中斷向量號*8加上IDT基底位址可以得到IDT內欲得到的中斷門描述符號，
###中斷門描述符內有GDT選擇子，可以選擇GDT內第幾個段描述符號，
###得到段基礎位址，然後把IDT內的偏移量+段基礎位址，得到中斷處理常式的實際位址。
*/

/*
###需要注意:
###	make_idt_desc函數影響的是中斷處理常式的偏移量，該函數位於idt_desc_init函數內，
###	所有的函數的偏移量為intr_entry_table，
###	除了系統呼叫的 0x80號中斷的 偏移量為 syscall_handler 之外，
###	因為系統呼叫的偏移量與眾不同，所以要在idt_desc_init函數內另外寫一個make_idt_desc函數。
###
###	exception_init函數影響的是進入到 intr_entry_table 後 call XXX會跑到哪裡，定義在interrupt.c
### syscall_init函數影響的是進入到 syscall_handler 後 call XXX會跑到哪裡，定義在syscall-init.c
*/