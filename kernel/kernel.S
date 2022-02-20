[bits 32]
%define ERROR_CODE nop	; 若在相关的异常中cpu已经自动压入了错误码,为保持栈中格式统一,这里不做操作.
%define ZERO push 0		; 若在相关的异常中cpu没有压入错误码,为了统一栈中格式,就手工压入一个0

;extern put_str			;声明外部函数
;##extern表示此函數已在其他檔案被定義
extern idt_table		 ;idt_table是C中注册的中断处理程序数组
section .data

;------------改進後的中斷處理常式不顯示"interrupt occur!"
;intr_str db "interrupt occur!", 0xa, 0

global intr_entry_table
intr_entry_table:


;=============================巨集定義==================================
%macro VECTOR 2
;###用macro定義了名為VECTOR的巨集，只接受兩個參數，
;###第1個參數是中斷向量號，第2個是要壓ERROR_CODE入棧還是壓入0，
;###為了能對齊，若此中斷沒有要壓ERROR_CODE，還是壓一個0進去。

section .text
intr%1entry:		; 每个中断处理程序都要压入中断向量号,所以一个中断类型一个中断处理程序，自己知道自己的中断向量号是多少
;###寫在中間的是%1，會被替換為第1個傳入巨集的參數，
;###此作法等同宣告了0x20=[0~32]=33個 intr_entry_table內 "一模一樣" 的東西，
;###(call [idt_table + %1*4]的idt_table的內容定義在interrupt.c，這條code除外)
;###例如33有個section .text、section .data和intrXXentry。
   
   %2
   ;###把%2替換為第2個傳入巨集的參數，
   ;###例如把%2換成ZERO，ZERO又被%define成push 0，
   ;###所以此時%2等於push 0。
   
   ; 以下是保存上下文环境
   push ds
   push es
   push fs
   push gs
   pushad			; PUSHAD指令压入32位寄存器,其入栈顺序是: EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
   
;--------以下註解掉的是改進前的中斷處理常式，此次不顯示"interrupt occur!"-----------
   ;push intr_str
   ;call put_str
   ;add esp,  4		; 收回傳入的参数
;-----------------------------------------------------------------------------------

   ; 如果是从片上进入的中断,除了往从片上发送EOI外,还要往主片上发送EOI
   ;##把EOI設置為手動結束
   mov al,0x20      ; 中断结束命令EOI
   out 0xa0,al      ; 向从片发送
   ;##0xa0為OCW2從片
   
   out 0x20,al      ; 向主片发送
   ;##0x20為OCW2主片
   
   push %1			; 不管idt_table中的目标程序是否需要参数,都一律压入中断向量号,调试时很方便
   call [idt_table + %1*4]; 调用idt_table中的C版本中断处理函数
;###呼叫定義在interrupt.c內的general_intr_handler函數，
;###general_intr_handler函數儲存在idt_table[IDT_DESC_CNT]內，共存0x21個，
   
   jmp intr_exit

;----------以下註解掉的是改進前的中斷處理常式--------------
   ;add esp,  4		; 跨过error_code
   ;##收回%2
   
   ;iret				; 从中断返回,32位下等同指令iretd

section .data
   dd  intr%1entry	; 存储各个中断入口程序的地址，形成intr_entry_table数组

%endmacro
;==========================================================================

section .text
global intr_exit
intr_exit:
   
; 以下是恢复上下文环境
   add esp, 4			   ; 跳过中断号
   popad
   pop gs
   pop fs
   pop es
   pop ds
   add esp, 4			   ; 跳过error_code
   iretd
   
   
;========================================================================
VECTOR 0x00,ZERO
VECTOR 0x01,ZERO
VECTOR 0x02,ZERO
VECTOR 0x03,ZERO 
VECTOR 0x04,ZERO
VECTOR 0x05,ZERO
VECTOR 0x06,ZERO
VECTOR 0x07,ZERO 
VECTOR 0x08,ERROR_CODE
VECTOR 0x09,ZERO
VECTOR 0x0a,ERROR_CODE
VECTOR 0x0b,ERROR_CODE 
VECTOR 0x0c,ZERO
VECTOR 0x0d,ERROR_CODE ;##GP例外
VECTOR 0x0e,ERROR_CODE
VECTOR 0x0f,ZERO 
VECTOR 0x10,ZERO
VECTOR 0x11,ERROR_CODE
VECTOR 0x12,ZERO
VECTOR 0x13,ZERO 
VECTOR 0x14,ZERO
VECTOR 0x15,ZERO
VECTOR 0x16,ZERO
VECTOR 0x17,ZERO 
VECTOR 0x18,ERROR_CODE
VECTOR 0x19,ZERO
VECTOR 0x1a,ERROR_CODE
VECTOR 0x1b,ERROR_CODE 
VECTOR 0x1c,ZERO
VECTOR 0x1d,ERROR_CODE
VECTOR 0x1e,ERROR_CODE
VECTOR 0x1f,ZERO 
VECTOR 0x20,ZERO
;###中斷處理常式有0x20+1個，有些會多壓error code入棧。

;~~~~~~~~~~~~~~~~第10章c~~~~~~~~~~~~~~~~~~~~~~
;###第十章c再添加16個中斷入口
VECTOR 0x21,ZERO	;键盘中断对应的入口
VECTOR 0x22,ZERO	;级联用的
VECTOR 0x23,ZERO	;串口2对应的入口
VECTOR 0x24,ZERO	;串口1对应的入口
VECTOR 0x25,ZERO	;并口2对应的入口
VECTOR 0x26,ZERO	;软盘对应的入口
VECTOR 0x27,ZERO	;并口1对应的入口
VECTOR 0x28,ZERO	;实时时钟对应的入口
VECTOR 0x29,ZERO	;重定向
VECTOR 0x2a,ZERO	;保留
VECTOR 0x2b,ZERO	;保留
VECTOR 0x2c,ZERO	;ps/2鼠标
VECTOR 0x2d,ZERO	;fpu浮点单元异常
VECTOR 0x2e,ZERO	;硬盘
VECTOR 0x2f,ZERO	;保留


;;;;;;;;;;;;;;;;   0x80号中断   ;;;;;;;;;;;;;;;;
[bits 32]
extern syscall_table	;###定義在syscall-init.c
section .text

global syscall_handler
syscall_handler:

;1 保存上下文环境
   push 0			    ; 压入0, 使栈中格式统一

   push ds
   push es
   push fs
   push gs
   pushad			    ; PUSHAD指令压入32位寄存器，其入栈顺序是:
						; EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI 
				 
   push 0x80			; 此位置压入0x80也是为了保持统一的栈格式

;###以下參照syscall.c的 "a" (NUMBER), "b" (ARG1), "c" (ARG2), "d" (ARG3)
;2 为系统调用子功能传入参数
   push edx			    ; 系统调用中第3个参数
   push ecx			    ; 系统调用中第2个参数
   push ebx			    ; 系统调用中第1个参数

;3 调用子功能处理函数
   call [syscall_table + eax*4]	; 编译器会在栈中根据C函数声明匹配正确数量的参数
   add esp, 12			; 收回上面的三个参数

;4 将call调用后的返回值存入待当前内核栈中eax的位置
   mov [esp + 8*4], eax	
   jmp intr_exit		; intr_exit返回,恢复上下文
