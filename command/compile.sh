###	此腳本應該在command目錄下執行		<====================重要!!!

###	由於windows默認的的換行符號是\r\n，
###	也就是說如果你在windows環境下寫code時，按下enter鍵，
###	會在HEX內自動加上\r\n，
###	但Linux默認的的換行符號是\n，
###	在Linux執行shell script時會把\r視為另外一個字元，
###	因此在Linux執行shell script前要輸入sed -i 's/\r//' compile.sh，
###
###	sed表示stream editor，流編輯器，
###	是一個使用簡單緊湊的程式語言來解析和轉換文字的Unix實用程式
###	-i表示in place，表示直接在該檔操作，
###	/s表示substitue，是ctrl+h的操作，即"取代"，
###	語法為:'/s/要被取代字元/取代後的字元/'
###	若 取代後的字元 是 / ， 
###	表示取代成 什麼都沒有，也就是把 要被取代字元 刪除，
###	所以sed -i 's/\r//' compile.sh的意思為:
###	把compile.sh內的\r字元刪除，
###	直接在compile.sh內操作，不另外輸出一個檔，
###
###	順便一提，Notepad++是把\n視為 回車換行，
###	所以\r\n會被Notepad++解讀為 回車再回車換行，
###	所以在Linux執行sed -i 's/\r//' compile.sh指令後，
###	Notepad++的解讀變為回車換行，
###	回車再回車換行 和 回車換行 的效果 都一樣，
###	所以Notepad++的顯示也不會變。

if [[ ! -d "../lib" || ! -d "../build" ]];then	#如果上一頁找不到lib或build目錄(表示很可能你現在未位於command目錄內)
   echo "dependent dir don\`t exist!"			#輸出"dependent dir don\`t exist!"到螢幕上，'\'表示下一個"撇"是字符，而不是shell script的符號
												#專業講法就是不進行轉義
   cwd=$(pwd)									#執行pwd(print working directory)命令，將原本要標準輸出到螢幕上的內容賦予cwd
   cwd=${cwd##*/}								#從cwd尋找匹配到"*/"的最長 開頭 ，把那開頭去掉
   cwd=${cwd%/}									#從cwd尋找匹配到"/" 的最短 結尾 ，把那結尾去掉(*路徑應該不會以"/"結尾，不知作者用義?)
												#關於 匹配 的詳解請見 "第十三章 Shell Script 程式設計" 之pdf檔的第13-29頁!!!
												#經過上面兩個去掉的步驟，現在當前路徑的最後一個name(現在位於的資料夾名稱)會被截取出來並存在cwd內
   if [[ $cwd != "command" ]];then				#如果當前路徑的最後一個name(現在位於的資料夾名稱)不為command
      echo -e "you\`d better in command dir\n"	#則輸出"you\`d better in command dir\n"，-e的意思解釋在最下面
   fi
   exit											#會進入到if表示上一頁找不到lib或build目錄，為不正常的狀況，所以在此用exit終止接下來的script運行
fi

#關於小、中、大括號的用法:
#https://codertw.com/%E7%A8%8B%E5%BC%8F%E8%AA%9E%E8%A8%80/544464/

BIN="prog_no_arg"
CFLAGS="-m32 -Wall -c -fno-builtin -W -Wstrict-prototypes \
      -Wmissing-prototypes -Wsystem-headers -fno-stack-protector"
LIB="../lib/"
OBJS="../build/string.o ../build/syscall.o \
      ../build/stdio.o ../build/assert.o"
#OBJS="../build/main.o ../build/init.o ../build/interrupt.o \
#	../build/timer.o ../build/kernel.o ../build/print.o \
#	../build/debug.o ../build/memory.o ../build/bitmap.o \
#	../build/string.o ../build/thread.o ../build/list.o \
#    ../build/switch.o ../build/console.o ../build/sync.o \
#	../build/keyboard.o ../build/ioqueue.o ../build/tss.o \
#	../build/process.o ../build/syscall.o ../build/syscall-init.o \
#	../build/stdio.o ../build/ide.o ../build/stdio-kernel.o ../build/fs.o \
#	../build/inode.o ../build/file.o ../build/dir.o  ../build/fork.o \
#	../build/shell.o ../build/assert.o ../build/buildin_cmd.o \
#	../build/exec.o"
DD_IN=$BIN
DD_OUT="hd3M.img" 

gcc $CFLAGS -I $LIB -o $BIN".o" $BIN".c"
ld -melf_i386 -Ttext 0x8008000 -e main $BIN".o" $OBJS -o $BIN
SEC_CNT=$(ls -l $BIN|awk '{printf("%d", ($5+511)/512)}')
#用ls -l prog_no_arg標準輸出prog_no_arg檔案的詳細資訊到管線內，
#prog_no_arg是二進制的可執行檔，有沒有加副檔名不影響執行，
#
#awk為處理文本的指令，其語法為:
#awk '條件類型1{動作1} 條件類型2{動作2} ...' filename
#參考網址：https://kknews.cc/code/lng854z.html
#awk指令的第二個參數為檔案名，
#此時未填檔案名，所以awk指令會去讀管線內的資訊，
#
#ls -l prog_no_arg會輸出一列的資訊，從左邊數來第五個項目是該檔案的大小，
#($5+511/512) <-- 用小括號包住數學算式，把檔案大小向上取整除以512表示該檔案要用到扇區數，
#
#經由以上統整，可知SEC_CNT=$(ls -l $BIN|awk '{printf("%d", ($5+511)/512)}')的意思為:
#先用ls獲得prog_no_arg的資訊並存入管線內，
#因為未填檔案名，所以awk要處理的檔案默認為"管線"，
#藉由 $5 獲得第5項資訊(prog_no_arg的大小)，
#將prog_no_arg的大小向上取整除以512已獲得prog_no_arg要用到扇區數，
#把prog_no_arg要用到扇區數賦予SEC_CNT。

if [[ -f $BIN ]];then
   dd if=./$DD_IN of=../$DD_OUT bs=512 \
   count=$SEC_CNT seek=300 conv=notrunc
fi

##########   以上核心就是下面这三条命令   ##########
#gcc -m32 -Wall -c -fno-builtin -W -Wstrict-prototypes -Wmissing-prototypes \
#    -Wsystem-headers -I ../lib -o prog_no_arg.o prog_no_arg.c
#ld -melf_i386 -e main_for_app prog_no_arg.o ../build/string.o ../build/syscall.o\
#   ../build/stdio.o ../build/assert.o -o prog_no_arg
#dd if=prog_no_arg of=../hd3M.img \
#   bs=512 count=10 seek=300 conv=notrunc


#-e 表示若字符串中出現以下字符，則特別加以處理，而不會將它當成一般文字輸出：	
#
#
#\a 發出警告聲；
#
#\b 刪除前一個字符；
#
#\c 最後不加上換行符號；
#
#\f 換行但光標仍舊停留在原來的位置；
#
#\n 換行且光標移至行首；
#
#\r 光標移至行首，但不換行；
#
#\t 插入tab；
#
#\v 與\f相同；
#
#\ 插入\字符；
#
#\nnn 插入nnn（八進位）所代表的ASCII字符；
#
#–help 顯示幫助
#
#–version 顯示版本信息
#
#
#原文網址：https://kknews.cc/code/b53mn4j.html