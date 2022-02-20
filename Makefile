BUILD_DIR = ./build
DISK_IMG = *.img
ENTRY_POINT = 0xc0001500
AS = nasm
CC = gcc
LD = ld
LIB = -I lib/ -I lib/kernel/ -I lib/user/ -I kernel/ -I device/ -I thread/ -I userprog/ -I fs/ -I shell/
ASFLAGS = -f elf 
ASBINLIB = -I boot/include/
CFLAGS = -m32 -Wall $(LIB) -c -fno-builtin -W -Wstrict-prototypes \
         -Wmissing-prototypes -fno-stack-protector
		###-32代表編譯在64位元平台上編譯32位元的程序
		###-Wall表示顯示所有警告
		###-c代表只編譯不連結
		###-fno-builtin為不採用內建函數
		###-W表示顯示編譯器認為會出現錯誤的警告	
		###-Wstrict-prototypes為要求函數宣告必須要有參數類型，否則發出警告
		###-Wmissing-prototypes為要求函數必須要有宣告，否則發出警告
		###在Ubuntu，gcc不加-fno-stack-protector連結器會出現錯誤
LDFLAGS = -melf_i386 -Ttext $(ENTRY_POINT) -e main -Map $(BUILD_DIR)/kernel.map
OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/init.o $(BUILD_DIR)/interrupt.o \
	$(BUILD_DIR)/timer.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/print.o \
	$(BUILD_DIR)/debug.o $(BUILD_DIR)/memory.o $(BUILD_DIR)/bitmap.o \
	$(BUILD_DIR)/string.o $(BUILD_DIR)/thread.o $(BUILD_DIR)/list.o \
    $(BUILD_DIR)/switch.o $(BUILD_DIR)/console.o $(BUILD_DIR)/sync.o \
	$(BUILD_DIR)/keyboard.o $(BUILD_DIR)/ioqueue.o $(BUILD_DIR)/tss.o \
	$(BUILD_DIR)/process.o $(BUILD_DIR)/syscall.o $(BUILD_DIR)/syscall-init.o \
	$(BUILD_DIR)/stdio.o $(BUILD_DIR)/ide.o $(BUILD_DIR)/stdio-kernel.o $(BUILD_DIR)/fs.o \
	$(BUILD_DIR)/inode.o $(BUILD_DIR)/file.o $(BUILD_DIR)/dir.o  $(BUILD_DIR)/fork.o \
	$(BUILD_DIR)/shell.o $(BUILD_DIR)/assert.o $(BUILD_DIR)/buildin_cmd.o

##############     MBR代码编译     ############### 
$(BUILD_DIR)/mbr.bin: boot/mbr.S 
	$(AS) $(ASBINLIB) $< -o $@

##############     bootloader代码编译     ###############
$(BUILD_DIR)/loader.bin: boot/loader.S 
	$(AS) $(ASBINLIB) $< -o $@

##############     c代码编译     ###############
$(BUILD_DIR)/main.o: kernel/main.c lib/kernel/print.h \
        lib/stdint.h kernel/init.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/init.o: kernel/init.c kernel/init.h lib/kernel/print.h \
        lib/stdint.h kernel/interrupt.h device/timer.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/interrupt.o: kernel/interrupt.c kernel/interrupt.h \
        lib/stdint.h kernel/global.h lib/kernel/io.h lib/kernel/print.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/timer.o: device/timer.c device/timer.h lib/stdint.h \
         lib/kernel/io.h lib/kernel/print.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/debug.o: kernel/debug.c kernel/debug.h \
        lib/kernel/print.h lib/stdint.h kernel/interrupt.h
	$(CC) $(CFLAGS) $< -o $@
	
$(BUILD_DIR)/string.o: lib/string.c lib/string.h \
        lib/stdint.h  kernel/debug.h  lib/string.h kernel/global.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/bitmap.o: lib/kernel/bitmap.c lib/kernel/bitmap.h \
        lib/kernel/print.h lib/stdint.h kernel/interrupt.h \
		kernel/debug.h  lib/string.h kernel/global.h
	$(CC) $(CFLAGS) $< -o $@

#$(BUILD_DIR)/memory.o: kernel/memory.c kernel/memory.h \
#        lib/kernel/bitmap.h lib/stdint.h 
#	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/memory.o: kernel/memory.c kernel/memory.h lib/stdint.h lib/kernel/bitmap.h \
		kernel/global.h kernel/global.h kernel/debug.h lib/kernel/print.h \
		lib/kernel/io.h kernel/interrupt.h lib/string.h lib/stdint.h
	$(CC) $(CFLAGS) $< -o $@
	
$(BUILD_DIR)/thread.o: thread/thread.c thread/thread.h lib/stdint.h \
	    kernel/global.h lib/kernel/bitmap.h kernel/memory.h lib/string.h \
		lib/stdint.h lib/kernel/print.h kernel/interrupt.h kernel/debug.h
	$(CC) $(CFLAGS) $< -o $@
	
$(BUILD_DIR)/list.o: lib/kernel/list.c lib/kernel/list.h kernel/global.h lib/stdint.h \
	        kernel/interrupt.h
		$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/console.o: device/console.c device/console.h lib/stdint.h \
	    lib/kernel/print.h thread/sync.h lib/kernel/list.h kernel/global.h \
		thread/thread.h thread/thread.h
		$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/sync.o: thread/sync.c thread/sync.h lib/kernel/list.h kernel/global.h \
	    lib/stdint.h thread/thread.h lib/string.h lib/stdint.h kernel/debug.h \
		kernel/interrupt.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/keyboard.o: device/keyboard.c device/keyboard.h lib/kernel/print.h \
	    lib/stdint.h kernel/interrupt.h lib/kernel/io.h thread/thread.h \
		lib/kernel/list.h kernel/global.h thread/sync.h thread/thread.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/ioqueue.o: device/ioqueue.c device/ioqueue.h lib/stdint.h thread/thread.h \
		lib/kernel/list.h kernel/global.h thread/sync.h thread/thread.h kernel/interrupt.h \
		kernel/debug.h
		$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/tss.o: userprog/tss.c userprog/tss.h thread/thread.h lib/stdint.h \
		lib/kernel/list.h kernel/global.h lib/string.h lib/stdint.h \
		lib/kernel/print.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/process.o: userprog/process.c userprog/process.h thread/thread.h \
		lib/stdint.h lib/kernel/list.h kernel/global.h kernel/debug.h \
		kernel/memory.h lib/kernel/bitmap.h userprog/tss.h kernel/interrupt.h \
		lib/string.h lib/stdint.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/syscall.o: lib/user/syscall.c lib/user/syscall.h lib/stdint.h
		$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/syscall-init.o: userprog/syscall-init.c userprog/syscall-init.h \
		lib/stdint.h lib/user/syscall.h lib/kernel/print.h thread/thread.h \
		lib/kernel/list.h kernel/global.h lib/kernel/bitmap.h kernel/memory.h
		$(CC) $(CFLAGS) $< -o $@	

$(BUILD_DIR)/stdio.o: lib/stdio.c lib/stdio.h lib/stdint.h kernel/interrupt.h \
		lib/stdint.h kernel/global.h lib/string.h lib/user/syscall.h lib/kernel/print.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/ide.o: device/ide.c device/ide.h lib/stdint.h thread/sync.h \
		lib/kernel/list.h kernel/global.h thread/thread.h lib/kernel/bitmap.h \
		kernel/memory.h lib/kernel/io.h lib/stdio.h lib/stdint.h lib/kernel/stdio-kernel.h\
		kernel/interrupt.h kernel/debug.h device/console.h device/timer.h lib/string.h
		$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/stdio-kernel.o: lib/kernel/stdio-kernel.c lib/kernel/stdio-kernel.h lib/stdint.h \
		lib/kernel/print.h lib/stdio.h lib/stdint.h device/console.h kernel/global.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/fs.o: fs/fs.c fs/fs.h lib/stdint.h device/ide.h thread/sync.h lib/kernel/list.h \
		kernel/global.h thread/thread.h lib/kernel/bitmap.h kernel/memory.h fs/super_block.h \
		fs/inode.h fs/dir.h lib/kernel/stdio-kernel.h lib/string.h lib/stdint.h kernel/debug.h \
		kernel/interrupt.h lib/kernel/print.h
		$(CC) $(CFLAGS) $< -o $@
		
$(BUILD_DIR)/inode.o: fs/inode.c fs/inode.h lib/stdint.h lib/kernel/list.h \
	kernel/global.h fs/fs.h device/ide.h thread/sync.h thread/thread.h \
	lib/kernel/bitmap.h kernel/memory.h fs/file.h kernel/debug.h \
	kernel/interrupt.h lib/kernel/stdio-kernel.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/file.o: fs/file.c fs/file.h lib/stdint.h device/ide.h thread/sync.h \
	lib/kernel/list.h kernel/global.h thread/thread.h lib/kernel/bitmap.h \
	kernel/memory.h fs/fs.h fs/inode.h fs/dir.h lib/kernel/stdio-kernel.h \
	kernel/debug.h kernel/interrupt.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/dir.o: fs/dir.c fs/dir.h lib/stdint.h fs/inode.h lib/kernel/list.h \
	kernel/global.h device/ide.h thread/sync.h thread/thread.h \
	lib/kernel/bitmap.h kernel/memory.h fs/fs.h fs/file.h \
	lib/kernel/stdio-kernel.h kernel/debug.h kernel/interrupt.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/fork.o: userprog/fork.c userprog/fork.h thread/thread.h lib/stdint.h \
	lib/kernel/list.h kernel/global.h lib/kernel/bitmap.h kernel/memory.h \
	userprog/process.h kernel/interrupt.h kernel/debug.h \
	lib/kernel/stdio-kernel.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/shell.o: shell/shell.c shell/shell.h lib/stdint.h fs/fs.h \
	lib/user/syscall.h lib/stdio.h lib/stdint.h kernel/global.h lib/user/assert.h
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/assert.o: lib/user/assert.c lib/user/assert.h lib/stdio.h lib/stdint.h
	$(CC) $(CFLAGS) $< -o $@
	
$(BUILD_DIR)/buildin_cmd.o: shell/buildin_cmd.c shell/buildin_cmd.h lib/stdint.h \
	lib/user/syscall.h lib/stdio.h lib/stdint.h lib/string.h fs/fs.h
	$(CC) $(CFLAGS) $< -o $@

##############    汇编代码编译    ###############
$(BUILD_DIR)/kernel.o: kernel/kernel.S
	$(AS) $(ASFLAGS) $< -o $@
	
$(BUILD_DIR)/print.o: lib/kernel/print.S
	$(AS) $(ASFLAGS) $< -o $@
	
$(BUILD_DIR)/switch.o: thread/switch.S
		$(AS) $(ASFLAGS) $< -o $@

##############    链接所有目标文件    #############
$(BUILD_DIR)/kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) $^ -o $@
	###$^為所有依賴檔案


.PHONY : mk_dir hd clean all
	###特別表明這些是虛擬目標，
	###這樣若不小心和"目標檔案:依賴檔案"的目標檔案撞名，虛擬目標的指令照樣可以正常執行

mk_dir:
	if [ ! -d $(BUILD_DIR) ];then mkdir $(BUILD_DIR);fi
	###判斷build目錄是不是存在，不存在則創一個

#mk_img:
#	if [ ! -e $(DISK_IMG) ];then /usr/bin/bximage -hd -mode="flat" -size=3 -q $(DISK_IMG);fi

hd:
	dd if=$(BUILD_DIR)/mbr.bin of=hd3M.img bs=512 count=1  conv=notrunc
	dd if=$(BUILD_DIR)/loader.bin of=hd3M.img bs=512 count=4 seek=2 conv=notrunc
	dd if=$(BUILD_DIR)/kernel.bin \
           of=hd3M.img \
           bs=512 count=200 seek=9 conv=notrunc

clean:
	cd $(BUILD_DIR) && rm -f ./*
	###進入build目錄後remove所有檔案

#clean:
#	cd $(BUILD_DIR) && rm -f ./* && rm ../$(DISK_IMG)

build: $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/mbr.bin $(BUILD_DIR)/loader.bin

all: mk_dir build hd
###依次執行mk_dir、build、hd，build要三個.bin都出現才會成立!