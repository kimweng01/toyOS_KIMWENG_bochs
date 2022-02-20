[bits 32]
section .text
global switch_to
switch_to:
;==栈中此处是返回地址==	       
   push esi
   push edi
   push ebx
   push ebp

   mov eax, [esp + 20]	; 得到栈中的参数cur, cur = [esp+20]
   mov [eax], esp		; 保存栈顶指针esp. task_struct的self_kstack字段,
   ; self_kstack在task_struct中的偏移为0,
   ; 所以直接往thread开头处存4字节便可。

;------------------  以上是备份当前线程的环境，下面是恢复下一个线程的环境  ----------------
   mov eax, [esp + 24]	; 得到栈中的参数next, next = [esp+24]
   mov esp, [eax]		; pcb的第一个成员是self_kstack成员,用来记录0级栈顶指针,
						; 用来上cpu时恢复0级栈,0级栈中保存了进程或线程所有信息,包括3级栈指针
   pop ebp
   pop ebx
   pop edi
   pop esi
   ret				 	; 返回到上面switch_to下面的那句注释的返回地址,
   ;###未由中断进入,第一次执行时会返回到kernel_thread!!!
   
;###特別注意，main是因為時間到被中斷交棒給A的，等於main重新獲得執行權限後，要回到main被中斷的程式處繼續執行，
;###main當初被中斷後，進入中斷處理常式，一路經過intr_timer_handler、schedule()，最後才走到switch_to，
;###所以當初main 被中斷後 是把自己跑進switch_to函數後的 esp 存在main的PCB的基底位址，
;###假如現在執行緒D因為時間到要把CPU的使用權限交回給main，則D進入中斷處理常式、intr_timer_handler、schedule()、
;###進入switch_to函數，把自己現在的esp存入自己的PCB的基底位址(cur)後，現在要去main的PCB的基底位址(next)拿main的esp，
;###
;###一個正在執行的執行緒交棒一定要經由時脈中斷以進入中斷處理常式，就是因為main執行過，
;###所以此時main的PCB的基底位址(next)存放的是: 當初main被中斷後，跑進switch_to函數後的 esp ，
;###等於D交棒給main後，esp回到main當初進入switch_to時的esp，
;###所以這時候esp+4~esp+12的esi edi ebx ebp也是當初main進入switch_to時壓入的值，
;###連esp+16也是main當初進入switch_to後，要從switch_to返回的位址，
;###因此執行緒D在switch_to交棒給main時，拿到當初esp的值 並 pop四個暫存器值後，
;###ret會回到當初呼叫switch_to的地方，也就是 schedule()函數 內，
;###
;###由於switch_to函數是schedule()內最後一個函數，所以接著會從schedule()返回，回到timer.c的intr_timer_handler函數，
;###此時intr_timer_handler的後面也沒東西了，所又會從intr_timer_handler返回，回到kernel.S，
;###再從kernel.S的iretd指令返回，回到main當初被時脈中斷的地方。