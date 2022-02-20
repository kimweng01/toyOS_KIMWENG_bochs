#ifndef __LIB_KERNEL_BITMAP_H
#define __LIB_KERNEL_BITMAP_H
#include "global.h"
#define BITMAP_MASK 1

struct bitmap {
   uint32_t btmp_bytes_len;
/* 在遍历位图时,整体上以字节为单位,细节上是以位为单位,所以此处位图的指针必须是单字节 */
   uint8_t* bits; //點陣圖的初始虛擬位址
};
/*	###描述點陣圖的struct只有兩個內容，這個struct表達的是"整個"點陣圖，
	###btmp_bytes_len表達的是"整個"點陣圖的長度，
	###btis是指標，點陣圖可以比喻成每8位元圍一個框框，
	###而btis[0]指的是第0個框框內的第0個位元的位址，
	###而btis[1]指的是第0個框框內的第0個位元的位址...。	*/

void bitmap_init(struct bitmap* btmp);
int bitmap_scan_test(struct bitmap* btmp, uint32_t bit_idx);
int bitmap_scan(struct bitmap* btmp, uint32_t cnt);
void bitmap_set(struct bitmap* btmp, uint32_t bit_idx, int8_t value);
#endif
