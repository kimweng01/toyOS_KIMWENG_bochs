   %include "boot.inc"
   section loader vstart=LOADER_BASE_ADDR
   LOADER_STACK_TOP equ LOADER_BASE_ADDR


;构建gdt及其内部的描述符
;###此位址為0x900為GDT的起始地址!!!
   GDT_BASE:   		dd    0x00000000 
					dd    0x00000000
		   
   CODE_DESC:  		dd    0x0000FFFF 
					dd    DESC_CODE_HIGH4

   DATA_STACK_DESC: dd    0x0000FFFF
					dd    DESC_DATA_HIGH4

   VIDEO_DESC: 		dd    0x80000007	             ;limit=(0xbffff-0xb8000)/4k=0x7
					dd    DESC_VIDEO_HIGH4           ;此时dpl为0

   GDT_SIZE    		equ   $ - GDT_BASE
   GDT_LIMIT   		equ   GDT_SIZE - 1 
   times 60 dq 0 						             ;此处预留60个描述符的空位(slot)
   SELECTOR_CODE  	equ (0x0001<<3) + TI_GDT + RPL0  ;相当于(CODE_DESC - GDT_BASE)/8 + TI_GDT + RPL0
   SELECTOR_DATA  	equ (0x0002<<3) + TI_GDT + RPL0	 ;同上
   SELECTOR_VIDEO 	equ (0x0003<<3) + TI_GDT + RPL0	 ;同上 

   ; total_mem_bytes用于保存内存容量,以字节为单位,此位置比较好记。
   ; 当前偏移loader.bin文件头0x200字节,loader.bin的加载地址是0x900,
   ; 故total_mem_bytes内存中的地址是0xb00.将来在内核中咱们会引用此地址
   ;###段描述符號大小是8位元組，dq也是8位元組，故當前偏移量為:
   ;###(4+60)*8=512=0x200
   total_mem_bytes dd 0		   
;----------------------------------------------------------------

   ;以下是定义gdt的指针，前2字节是gdt界限，后4字节是gdt起始地址
   gdt_ptr  	   dw  GDT_LIMIT 
			       dd  GDT_BASE

   ;人工对齐:total_mem_bytes4字节+gdt_ptr6字节+ards_buf244字节+ards_nr2,共256字节
   ards_buf times 244 db 0 ;存傳回來的ARDS用的
   ards_nr 	dw 0		   ;用于记录ards结构体数量



;######################################################################################################
   loader_start: ;loader正式啟動!
   
;==================int 15h eax = 0000E820h ,edx = 534D4150h ('SMAP') 获取内存布局===============
   ;###此位置偏移量為0x300!!!
   xor ebx, ebx		       ;第一次调用时，ebx值要为0
   mov edx, 0x534d4150	   ;edx只赋值一次，循环体中不会改变
   mov di, 	ards_buf	   ;ards结构缓冲区
  ;##int後，di儲存會儲存現在的ARDS

.e820_mem_get_loop:	       ;循环获取每个ARDS内存范围描述结构
   mov eax, 0x0000e820	   ;执行int 0x15后,eax值变为0x534d4150,所以每次执行int前都要更新为子功能号。
   mov ecx, 20		       ;ARDS地址范围描述符结构大小是20字节
   ;##eax和ecx每次都要更新
   
   int 0x15
   jc .e820_failed_so_try_e801   ;若cf位为1则有错误发生，尝试0xe801子功能
   add di, 	cx		      ;使di增加20字节指向缓冲区中新的ARDS结构位置
   inc word [ards_nr]	  ;记录ARDS数量
   cmp ebx, 0		      ;若ebx为0且cf不为1,这说明ards全部返回，当前已是最后一个
   ;int後，ebx會記錄下一個待回傳的ARDS，若為0，表示現在已經是最後一個了
   
   jnz .e820_mem_get_loop

;在所有ards结构中，找出(base_add_low + length_low)的最大值，即内存的容量。
;##得到的最大記憶體容量的base_add_low應該是從0開始算起? 所以base_add_low + length_low的最大值，就會是内存的容量!
   mov cx, 	[ards_nr]	   ;遍历每一个ARDS结构体,循环次数是ARDS的数量
   mov ebx, ards_buf 
   xor edx, edx		       ;edx为最大的内存容量,在此先清0
   
.find_max_mem_area:	       ;无须判断type是否为1,最大的内存块一定是可被使用
   mov eax, [ebx]	       ;base_add_low
   add eax, [ebx+8]	       ;length_low
   add ebx, 20		       ;指向缓冲区中下一个ARDS结构
   cmp edx, eax		       ;冒泡排序，找出最大,edx寄存器始终是最大的内存容量
   jge .next_ards
   mov edx, eax		       ;edx为总内存大小

.next_ards:
   loop .find_max_mem_area
   jmp  .mem_get_ok


;========================int 15h ax = E801h 获取内存大小,最大支持4G===========================
; 返回后, ax cx 值一样,以KB为单位,bx dx值一样,以64KB为单位
; 在ax和cx寄存器中为低16M,在bx和dx寄存器中为16MB到4G。
.e820_failed_so_try_e801:
   mov ax,	0xe801
   int 0x15
   jc  .e801_failed_so_try88;若当前e801方法失败,就尝试0x88方法

;---------1 先算出低15M的内存,ax和cx中是以1KB为单位的内存数量,将其转换为以byte为单位------
;###乘完後，dx存高16位元，先左移16位，而ax存低16位元，and eax,0x0000FFFF擷取低16位(怕eax的後16位元有殘餘值)
   mov cx,  0x400	    	 ;0x400=1KB，cx和ax值一样,cx用做乘数
   mul cx                    
   shl edx, 16               
   and eax, 0x0000FFFF       
   or  edx,	eax              
   add edx, 0x100000 	   	 ;ax只是15MB,故要加1MB
   mov esi,	edx	     	   	 ;先把低15MB的内存容量存入esi寄存器备份

;---------2 再将16MB以上的内存转换为byte为单位,寄存器bx和dx中是以64KB为单位的内存数量-------
   xor eax,	eax
   mov ax,	bx		
   mov ecx, 0x10000		   	 ;0x10000十进制为64KB
   mul ecx				   	 ;32位乘法,默认的被乘数是eax,积为64位,高32位存入edx,低32位存入eax.
   add esi,	eax			   	 ;由于此方法只能测出4G以内的内存,故32位eax足够了,edx肯定为0,只加eax便可
   mov edx,	esi			   	 ;edx为总内存大小
   jmp .mem_get_ok


;===========================int 15h ah = 0x88 获取内存大小,只能获取64M之内=========================
.e801_failed_so_try88: 
   ;##int 15后，ax存入的是以kb为单位的内存容量!!!
   mov ah,  0x88
   int 0x15
   jc  .error_hlt
   and eax, 0x0000FFFF
      
   ;16位乘法，被乘数是ax,积为32位.积的高16位在dx中，积的低16位在ax中
   mov cx,  0x400     		 ;0x400等于1024,将ax中的内存容量换为以byte为单位
   mul cx                    
   shl edx, 16	     		 ;把dx移到高16位
   or  edx, eax	     		 ;把积的低16位组合到edx,为32位的积
   add edx, 0x100000  		 ;0x88子功能只会返回1MB以上的内存,故实际内存大小要加上1MB

;==========================已獲得記憶體容量==========================
.mem_get_ok:
   mov [total_mem_bytes], edx;将内存换为byte单位后存入total_mem_bytes处。


;==========================准备进入保护模式===========================
;1 打开A20
;2 加载gdt
;3 将cr0的pe位置1
   ;-----------------  打开A20  ----------------
   in  al,	0x92
   or  al,	0000_0010B
   out 0x92,al

   ;-----------------  加载GDT  ----------------
   lgdt [gdt_ptr]

   ;-----------------  cr0第0位置1  ----------------
   mov eax, cr0
   or  eax, 0x00000001
   mov cr0, eax

   jmp dword SELECTOR_CODE:p_mode_start	; 刷新流水线，避免分支预测的影响,这种cpu优化策略，最怕jmp跳转，
   ;这将导致之前做的预测失效，从而起到了刷新的作用。
   ;###目前保護模式(32位元模式)已經開啟，SELECTOR_CODE是選擇子，選擇子內的索引值是1，
   ;###使CPU會根據GDT暫存器找出GDT的基底位址，然後查GDT的第1個段描述符號內寫的段基礎地址，
   ;###然後跳到查到的段基礎地址，此段基礎位址為0，再加上p_mode_start為實際跳到的位址。
   
.error_hlt:		      		 			;出错则挂起
   hlt



;###########################################################################################################
[bits 32] ;開始32位元模式! 進入前最好用jmp刷新管線
p_mode_start:
   mov ax, 	SELECTOR_DATA
   mov ds, 	ax
   mov es, 	ax
   mov ss, 	ax
   mov esp,	LOADER_STACK_TOP
   ;##LOADER_STACK_TOP=0x900
   mov ax, 	SELECTOR_VIDEO
   mov gs, 	ax
 
 
;============================================加载kernel============================================ 
   mov eax, KERNEL_START_SECTOR        ; kernel.bin所在的扇区号								
   ;##KERNEL_START_SECTOR=0x9																
   mov ebx, KERNEL_BIN_BASE_ADDR       ; 从磁盘读出后，写入到ebx指定的地址					
   ;##KERNEL_BIN_BASE_ADDR=0x70000                                                       
   mov ecx, 200			       ; 读入的扇区数                                               
																							
   call rd_disk_m_32                                                                        


;===============================创建页目录及页表并初始化页内存位图================================
   call setup_page

   ;要将描述符表地址及偏移量写入内存gdt_ptr,一会用新地址重新加载
   sgdt [gdt_ptr]	      ; 存储到原来gdt所有的位置
   ;##把存在CPU內的GDT暫存器載回[gdt_ptr]

   ;将gdt描述符中视频段描述符中的段基址+0xc0000000
   mov ebx, [gdt_ptr + 2]
   ;##把GDT_BASE的值存入ebx
   
   ;###			GDT暫存器:
   ;###		______________________
   ;###	   |  GDT_BASE	|GDT_LIMIT|
   ;###		￣￣￣￣￣￣￣￣￣￣￣
   ;###	   47		  16 15		  0
   ;###				   ^gdt_ptr+2 ^gdt_ptr
   
   or dword [ebx + 0x18 + 4], 0xc0000000      	;视频段是第3个段描述符,每个描述符是8字节,故0x18。
   ;段描述符的高4字节的最高位是段基址的31~24位
   ;##0x18=24，GDT_BASE+24即指向VIDEO的段描述符號的低32位，再+4為段描述符號高32位

   ;##将gdt的基址(GDT_BASE)加上0xc0000000使其成为内核所在的高地址
   add dword [gdt_ptr + 2], 0xc0000000

   add esp, 0xc0000000        					;将栈指针同样映射到内核地址


;=======================================開啟分頁模式=====================================================
   ; 把页目录地址赋给cr3
   mov eax, PAGE_DIR_TABLE_POS
   ;##PAGE_DIR_TABLE_POS=0x10000
   mov cr3, eax

   ; 打开cr0的pg位(第31位)
   mov eax, cr0
   or  eax, 0x80000000
   mov cr0, eax

   ;在开启分页后,用gdt新的地址重新加载
   lgdt [gdt_ptr]             ;重新加载

;---------------------------------------------------------------------------------------
   ;mov byte [gs:160], 'V'     ;视频段段基址已经被更新為0xc00b8000,用字符v表示virtual addr
   ;##gs <== SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0
   ;###gs裡面存的是選擇子，其索引值為3，CPU會去查GDT暫存器以得知GDT的起始位置在哪裡，
   ;###得到的GDT起始地址為0xc0000900，再經由分頁系統洗成0x00000900，
   ;###然後查位於GDT內的第3個段描述符號，得知VIDEO段的起始地址為0xc00b8000，
   ;###此起始地址又被分頁系統洗成0x000b8000，此值再+160，即'V'的最終顯示位置。

   ;jmp $
;---------------------------------------------------------------------------------------
;	^上面註解掉的code是上個程式的內容，##和###還是看一下!
   
   mov byte [gs:160], 'V'
   mov byte [gs:162], 'i'
   mov byte [gs:164], 'r'
   mov byte [gs:166], 't'
   mov byte [gs:168], 'u'
   mov byte [gs:170], 'a'
   mov byte [gs:172], 'l'
   
;;;;;;;;;;;;;;;;;;;;;;;;;;;;  此时不刷新流水线也没问题  ;;;;;;;;;;;;;;;;;;;;;;;;
;由于一直处在32位下,原则上不需要强制刷新,经过实际测试没有以下这两句也没问题.
;但以防万一，还是加上啦，免得将来出来莫句奇妙的问题.
   jmp SELECTOR_CODE:enter_kernel	  ;强制刷新流水线,更新gdt
   

enter_kernel:    
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
   mov byte [gs:320], 'k'
   mov byte [gs:322], 'e'
   mov byte [gs:324], 'r'
   mov byte [gs:326], 'n'
   mov byte [gs:328], 'e'
   mov byte [gs:330], 'l'

   mov byte [gs:480], 'w'
   mov byte [gs:482], 'h'
   mov byte [gs:484], 'i'
   mov byte [gs:486], 'l'
   mov byte [gs:488], 'e'
   mov byte [gs:490], '('
   mov byte [gs:492], '1'
   mov byte [gs:494], ')'
   mov byte [gs:496], ';'
   
   call kernel_init
   
   mov esp, 0xc009f000
   ;###把esp設在0xc009f000，以後堆疊都由此開始，會選0xc009f000的原因是要對齊4KB
   
   jmp KERNEL_ENTRY_POINT                 ; 用地址0x1500访问测试，结果ok
   ;##KERNEL_ENTRY_POINT=0xc0001500


;====================================将kernel.bin中的segment拷贝到编译的地址=================================
kernel_init:
   xor eax, eax
   xor ebx, ebx		;ebx记录程序头表地址
   xor ecx, ecx		;cx记录程序头表中的program header数量
   xor edx, edx		;dx 记录program header尺寸,即e_phentsize

   mov dx,  [KERNEL_BIN_BASE_ADDR + 42]	  ; 偏移文件42字节处的属性是e_phentsize,表示program header大小
   mov ebx, [KERNEL_BIN_BASE_ADDR + 28]   ; 偏移文件开始部分28字节的地方是e_phoff,表示第1 个program header在文件中的偏移量
					  ; 其实该值是0x34,不过还是谨慎一点，这里来读取实际值
   add ebx, KERNEL_BIN_BASE_ADDR
   mov cx,  [KERNEL_BIN_BASE_ADDR + 44]   ; 偏移文件开始部分44字节的地方是e_phnum,表示有几个program header

.each_segment:
   cmp byte   [ebx + 0], PT_NULL		  ; 若p_type等于 PT_NULL,说明此program header未使用。
   je  .PTNULL

;-----------为函数memcpy压入参数,参数是从右往左依然压入.函数原型类似于 memcpy(dst,src,size)---------
   push dword [ebx + 16]		  		  ; program header中偏移16字节的地方是p_filesz,压入函数memcpy的第三个参数:size
   
   mov  eax,  [ebx + 4]			          ; 距程序头偏移量为4字节的位置是p_offset
   add  eax,  KERNEL_BIN_BASE_ADDR	      ; 加上kernel.bin被加载到的物理地址,eax为该段的物理地址
   push eax				  				  ; 压入函数memcpy的第二个参数:源地址
   
   push dword [ebx + 8]			  		  ; 压入函数memcpy的第一个参数:目的地址,偏移程序头8字节的位置是p_vaddr，这就是目的地址
   
   call mem_cpy				  			  ; 调用mem_cpy完成段复制
   ;###調用mem_cpy準備把elf裡面的段的內容複製到它應該要在的位置，
   ;###需要三個參數，依序將段的size、寫在elf內的段的起始位置、段應該在的地址(目的地址)先押入棧。
   
   add  esp, 12				 			  ; 清理栈中压入的三个参数

.PTNULL:
   add  ebx, edx			     		  ; edx为program header大小,即e_phentsize,在此ebx指向下一个program header 
   ;##dx在上方的mov dx, [KERNEL_BIN_BASE_ADDR + 42]處被賦值
   loop .each_segment
   ;##cx在上面被賦值，記錄有幾個program header，等於loop .each_segment要迴圈的數量。
   ret

;----------  逐字节拷贝 mem_cpy(dst,src,size) ------------
;输入:栈中三个参数(dst,src,size)
;输出:无
;---------------------------------------------------------
mem_cpy:		      
   cld
   push ebp
   mov  ebp, esp
   push ecx		           ; rep指令用到了ecx，但ecx对于外层段的循环还有用，故先入栈备份
   mov  edi, [ebp + 8]	   ; dst
   mov  esi, [ebp + 12]	   ; src
   mov  ecx, [ebp + 16]	   ; size
   rep  movsb		       ; 逐字节拷贝

   ;恢复环境
   pop  ecx		
   pop  ebp
   ret

;##################################################################################################
;======================================创建页目录及页表==============================
setup_page:
;先把页目录占用的空间逐字节清0
   mov  ecx, 4096
   mov  esi, 0
   
.clear_page_dir:
   mov  byte [PAGE_DIR_TABLE_POS + esi], 0 
   ;##PAGE_DIR_TABLE_POS=0x100000，先把0x100000往上4KB全部清0
   inc  esi
   loop .clear_page_dir

;开始创建页目录项(PDE)
.create_pde:				     ; 创建Page Directory Entry
   mov  eax, PAGE_DIR_TABLE_POS
   add  eax, 0x1000 			 ; 此时eax为第一个页表的位置及属性
   mov  ebx, eax				 ; 此处为ebx赋值，是为.create_pte做准备，ebx为基址。
   ;##把0x101000存入ebx

;   下面将页目录项0和0xc00都存为第一个页表的地址，
;   一个页表可表示4MB内存,这样0xc03fffff以下的地址和0x003fffff的地址都指向相同的分页表，
;   这是为将地址映射为内核地址做准备
;###0xc03fffff=1100000000_1111111111_111111111111
   
   or   eax, PG_US_U | PG_RW_W | PG_P	      ; 页目录项的属性RW和P位为1,US为1,表示用户属性,所有特权级别都可以访问.
   ;##此時eax為0x101000
   
   mov  [PAGE_DIR_TABLE_POS + 0x0],   eax     ; 第1个目录项,在页目录表中的第1个目录项写入第一个页表的位置(0x101000)及属性(7)
   ;##把0x101007寫進0x100000
   
   mov  [PAGE_DIR_TABLE_POS + 0xc00], eax     ; 一个页表项占用4字节,0xc00表示第768个页表占用的目录项,0xc00以上的目录项用于内核空间,
   ; 也就是页表的0xc0000000~0xffffffff共计1G属于内核,0x0~0xbfffffff共计3G属于用户进程.
   ;##把0x101007寫進0x100c00
   
   sub  eax, 0x1000
   mov  [PAGE_DIR_TABLE_POS + 4092],  eax	  ; 使最后一个目录项指向页目录表自己的地址
   ;##把0x100007寫進0x100ffc

;下面创建页表项(PTE)
   mov  ecx, 256				     ; 1M低端内存 / 每页大小4k = 256
   mov  esi, 0
   mov  edx, PG_US_U | PG_RW_W | PG_P; 属性为7,US=1,RW=1,P=1

.create_pte:				         ; 创建Page Table Entry
   ;ebx=0x101000
   mov  [ebx+esi*4], edx			 ; 此时的ebx已经在上面通过eax赋值为0x101000,也就是第一个页表的地址 
   add  edx,4096
   inc  esi
   loop .create_pte

;创建内核其它页表的PDE
   mov  eax, PAGE_DIR_TABLE_POS
   add  eax, 0x2000 		     		; 此时eax为第二个页表的位置
   or   eax, PG_US_U | PG_RW_W | PG_P	; 页目录项的属性US,RW和P位都为1
   mov  ebx, PAGE_DIR_TABLE_POS
   mov  ecx, 254			    		; 范围为第769~1022的所有目录项数量
   mov  esi, 769

.create_kernel_pde:
   mov  [ebx+esi*4], eax
   inc  esi
   add  eax, 0x1000
   loop .create_kernel_pde
   ret

;####關於分頁的配置，請看筆記照相


;==============================================讀取OS=====================================================
;功能:读取硬盘n个扇区
rd_disk_m_32:	   
					   ; eax=LBA扇区号
					   ; ebx=将数据写入的内存地址
					   ; ecx=读入的扇区数
      mov esi,eax	   ; 备份eax
      mov di,cx		   ; 备份扇区数到di

;--------------------------------------------------
;读写硬盘:
;第1步：设置要读取的扇区数
      mov dx, 0x1f2
      mov al, cl
	  ;##cx此時為200  
      out dx, al       ;读取的扇区数

      mov eax,esi	   ;恢复ax

;第2步：将LBA地址存入0x1f3 ~ 0x1f6

      ;LBA地址7~0位写入端口0x1f3
      mov dx, 0x1f3                       
      out dx, al                          

      ;LBA地址15~8位写入端口0x1f4
      mov cl, 8
      shr eax,cl
      mov dx, 0x1f4
      out dx, al

      ;LBA地址23~16位写入端口0x1f5
      shr eax,cl
      mov dx, 0x1f5
      out dx, al

      shr eax,cl
      and al, 0x0f	   ;lba第24~27位
      or  al, 0xe0	   ; 设置7～4位为1110,表示lba模式
      mov dx, 0x1f6
      out dx, al

;第3步：向0x1f7端口写入读命令，0x20 
      mov dx, 0x1f7
      mov al, 0x20                        
      out dx, al

;;;;;;; 至此,硬盘控制器便从指定的lba地址(eax)处,读出连续的cx个扇区,下面检查硬盘状态,不忙就能把这cx个扇区的数据读出来

;第4步：检测硬盘状态
  .not_ready:		   ;测试0x1f7端口(status寄存器)的的BSY位
      ;同一端口,写时表示写入命令字,读时表示读入硬盘状态
      nop
      in  al, dx
      and al, 0x88	   ;第4位为1表示硬盘控制器已准备好数据传输,第7位为1表示硬盘忙
      cmp al, 0x08
      jnz .not_ready   ;若未准备好,继续等。

;第5步：从0x1f0端口读数据
      mov ax, di	   ;以下从硬盘端口读数据用insw指令更快捷,不过尽可能多的演示命令使用,
			           ;在此先用这种方法,在后面内容会用到insw和outsw等

      mov dx, 256	   ;di为要读取的扇区数,一个扇区有512字节,每次读入一个字,共需di*512/2次,所以di*256
      mul dx
      mov cx, ax	   
      mov dx, 0x1f0
	  
  .go_on_read:
      in  ax,    dx		
      mov [ebx], ax
	  ;##ebx=KERNEL_BIN_BASE_ADDR=0xc0001500

      add ebx, 2
			  ; 由于在实模式下偏移地址为16位,所以用bx只会访问到0~FFFFh的偏移。
			  ; loader的栈指针为0x900,bx为指向的数据输出缓冲区,且为16位，
			  ; 超过0xffff后,bx部分会从0开始,所以当要读取的扇区数过大,待写入的地址超过bx的范围时，
			  ; 从硬盘上读出的数据会把0x0000~0xffff的覆盖，
			  ; 造成栈被破坏,所以ret返回时,返回地址被破坏了,已经不是之前正确的地址,
			  ; 故程序出会错,不知道会跑到哪里去。
			  ; 所以改为ebx代替bx指向缓冲区,这样生成的机器码前面会有0x66和0x67来反转。
			  ; 0X66用于反转默认的操作数大小! 0X67用于反转默认的寻址方式.
			  ; cpu处于16位模式时,会理所当然的认为操作数和寻址都是16位,处于32位模式时,
			  ; 也会认为要执行的指令是32位.
			  ; 当我们在其中任意模式下用了另外模式的寻址方式或操作数大小(姑且认为16位模式用16位字节操作数，
			  ; 32位模式下用32字节的操作数)时,编译器会在指令前帮我们加上0x66或0x67，
			  ; 临时改变当前cpu模式到另外的模式下.
			  ; 假设当前运行在16位模式,遇到0X66时,操作数大小变为32位.
			  ; 假设当前运行在32位模式,遇到0X66时,操作数大小变为16位.
			  ; 假设当前运行在16位模式,遇到0X67时,寻址方式变为32位寻址
			  ; 假设当前运行在32位模式,遇到0X67时,寻址方式变为16位寻址.
	;###簡單說就是mov [ebx], ax如果寫成mov [bx], ax的話，如果目標位址超過0xffff就會壞掉，
	;###故寫成mov [ebx], ax，而由於現在已經是在32位元模式下了，而ax是16位元專用，
	;###所以編譯出來的指令前面會有0x66。

      loop .go_on_read
      ret

;###需要注意:	  
;###	[bits 32]以前，組譯器用16位元的方式翻譯成一堆1和0的機器碼
;###	[bits 32]以後，組譯器用32位元的方式翻譯成一堆1和0的機器碼
;###	整份翻譯完，CPU開始執行
;###	
;###	如果沒有jmp  SELECTOR_CODE:p_mode_start，
;###	mov cr0, eax正要執行，mov ax，SELECTOR_DATA已經在解碼了
;###	因為mov cr0, eax以後32解碼模式才會正式開通，
;###	所以已經翻成32位元模式的機器碼的mov ax，SELECTOR_DATA會按照16位元的方式解碼，
;###	這會導致執行時讀到的解碼值不正確
;###	所以用jmp  SELECTOR_CODE:p_mode_start來flush管線，
;###	把已經在管線內的mov ax, SELECTOR_DATA洗掉，再讓mov ax, SELECTOR_DATA重新進入管線執行
;###	
;###	關於Flag暫存器，以及GDT、LDT存取相關的問題，請見筆記照相
;###	和https://github.com/doctording/os/tree/master/docs/content/03_protected_mode
;###
;###	OS的第一段的虛擬位址位於0xc0001000，
;###	而程式真正開始的位址位於0xc0001500，
;###	而第一段的長度為0x505位元組，
;###	所以由此可知，第一段真正能執行的程式大小為0x5位元組，
;###	p_offset值為0，表示說第一段包括elf header和program header，
;###	所以把kernel複製到它應該要在的位置時，會連elf header和program header也都複製過去，
;###	不過沒有關係，因為程式真正開始的位址位於0xc0001500，
;###	也就是說程式真正開始執行的位置不是從第一段的虛擬起始位址0xc0001000開始跑，
;###	等於是跨過了elf header和program header，所以不會出問題。