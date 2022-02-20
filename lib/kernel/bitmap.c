#include "bitmap.h"
#include "stdint.h"
#include "string.h"
#include "print.h"
#include "interrupt.h"
#include "debug.h"

/*	###描述點陣圖的struct只有兩個內容，這個struct表達的是"整個"點陣圖，
	###btmp_bytes_len表達的是"整個"點陣圖的長度，以位元組為單位，
	###btis是指標，點陣圖可以比喻成每8位元圍一個框框，
	###而btis[0]指的是第0個框框內的第0個位元的位址，
	###而btis[1]指的是第0個框框內的第0個位元的位址...。	*/

/* 将位图btmp初始化 */
void bitmap_init(struct bitmap* btmp) {
   memset(btmp->bits, 0, btmp->btmp_bytes_len);   
}

/* 判断bit_idx位是否为1,若为1则返回true，否则返回false */
bool bitmap_scan_test(struct bitmap* btmp, uint32_t bit_idx) {
   uint32_t byte_idx = bit_idx / 8;    // 向下取整用于索引数组下标
   uint32_t bit_odd  = bit_idx % 8;    // 取余用于索引数组内的位
   return (btmp->bits[byte_idx] & (BITMAP_MASK << bit_odd));
}
/*	###指標最小就是指向1位元組的指標，內有8位元，
	###很像是每8位元圍一個方框，每個方框內有8位元，
	###意即bits[0]為第0個方框的位址，bits[1]為第1個方框的位址。
	###若要判斷bit_idx是否為1，假如bit_idx=27，
	###則27 / 8	= 3...2，表示第27位位在第3個方框內的8個位元中的第2個位元(從0開始算)，
	###BITMAP_MASK=0x00000001，往左挪2位數(所求得的餘數值)變成0x00000100，
	###然後把0x00000100跟第3個方框中的8個位元做and運算，
	###即可判斷第27位(第3號方框內的第2位置)是否為1。	*/

//===============##在位图中申请连续cnt个位,成功则返回其起始位下标，失败返回=============//
/*	###法一:
	###先連續找至少有一個0的"方框"，找到後就不再使用方框搜尋法，
	###然後以該方框內的第0個位元為起點，一個一個位元搜尋，
	###直到找到連續的0的數量=cnt為止。	*/
int bitmap_scan(struct bitmap* btmp, uint32_t cnt) {
   uint32_t idx_byte = 0;	 // 用于记录空闲位所在的字节
/* 先逐字节比较,蛮力法 */
   while ( (0xff == btmp->bits[idx_byte]) && (idx_byte < btmp->btmp_bytes_len) ) {
/* ##1表示该位已分配,所以若为0xff,则表示该字节内已无空闲位,向下一字节继续找 */
      idx_byte++;
	  //##idx_byte單位是位元組
   }

   ASSERT(idx_byte < btmp->btmp_bytes_len); //##false會ASSERT
   if (idx_byte == btmp->btmp_bytes_len) {  // 若该内存池找不到可用空间		
      return -1;
   }

 /* ##若在位图数组范围内的某字节内找到了空闲位，
  * ##在该字节内逐位比对,返回空闲位的索引。*/
   int idx_bit = 0;
 /* 和btmp->bits[idx_byte]这个字节逐位对比 */
   while ((uint8_t)(BITMAP_MASK << idx_bit) & btmp->bits[idx_byte]) { 
	 idx_bit++;
   }
	 
   int bit_idx_start = idx_byte * 8 + idx_bit;    // 空闲位在位图内的下标
   if (cnt == 1) {
      return bit_idx_start;
   }

   uint32_t bit_left = (btmp->btmp_bytes_len * 8 - bit_idx_start);   // 记录还有多少位可以判断
   uint32_t next_bit = bit_idx_start + 1;
   uint32_t count = 1;	      // 用于记录找到的空闲位的个数

   bit_idx_start = -1;	      // 先将其置为-1,若找不到连续的位就直接返回
   while (bit_left-- > 0) {
      if ( !(bitmap_scan_test(btmp, next_bit)) ) {	 // 若next_bit为0
		count++;
      } 
	  else {
		count = 0;
      }
	  
      if (count == cnt) {	    // 若找到连续的cnt个空位
		bit_idx_start = next_bit - cnt + 1;
		break;
      }
      next_bit++;          
   }
   return bit_idx_start;
}

/*
###法二:
###最為暴力的方法，直接一個一個位元搜尋，
###直到找到連續的0的數量=cnt為止。
int bitmap_scan(struct bitmap* btmp, uint32_t cnt) {
	uint32_t idx_byte = 0;
	int count = 0;
	int btmp_all_len = btmp->btmp_bytes_len*8;
	
	for (int bit_idx_start=0; bit_idx_start<btmp_all_len; bit_idx_start++) {
		if ( !(bitmap_scan_test(btmp, next_bit)) ) { 
		//bit_idx_start的單位是1 bit，判斷該bit是否為1要送入bitmap_scan_test函數判斷
			count ++;
			if(count == cnt)
				return bit_idx_start - cnt + 1;
		}
		else
			count = 0;
	}
	
	ASSERT(idx_byte < btmp->btmp_bytes_len);
	return -1;
}
*/

/*
###法三:
###先連續找至少有一個0的"方框"，
###找到後就以該方框內的第0個位元為起點，一個一個位元搜尋，
###設法找到連續的0 且 0的數量=cnt，
###若現在正在搜尋的位元剛好是某個方框內的第0個位元 且 該位元為1，
###表示目前已經沒有連續的0"橫跨"兩個方框，所以從"一個一個位元搜尋法"break，
###回到"方框搜尋法"繼續搜尋。
int bitmap_scan(struct bitmap* btmp, uint32_t cnt) {
	int idx_bit = 0;
	int bit_idx_start;
	int btmp_all_len = btmp->btmp_bytes_len*8;
	uint32_t count = 0;
	
	for (idx_byte=0; idx_byte < btmp->btmp_bytes_len; idx_byte++) {
		if(0xff != btmp->bits[idx_byte]) {
			while ((uint8_t)(BITMAP_MASK << idx_bit) & btmp->bits[idx_byte])
				idx_bit ++;
		}
		
		bit_idx_start = idx_byte * 8 + idx_bit;
		
		for (; bit_idx_start<btmp_all_len; bit_idx_start++) {
			if ( !(bitmap_scan_test(btmp, next_bit)) ) { 
			//bit_idx_start的單位是1 bit，判斷該bit是否為1要送入bitmap_scan_test函數判斷
				count ++;
				if(count == cnt)
					return bit_idx_start - cnt + 1;
			}
			else
				count = 0;
			
			if(bit_idx_start%8 == 0  &&  true == bitmap_scan_test(btmp, bit_idx_start))
				break;
		}
		
		idx_byte = bit_idx_start/8;
	}
	
	ASSERT(idx_byte < btmp->btmp_bytes_len);
	return -1;
}
*/

/* 将位图btmp的bit_idx位设置为value */
void bitmap_set(struct bitmap* btmp, uint32_t bit_idx, int8_t value) {
   ASSERT((value == 0) || (value == 1));
   uint32_t byte_idx = bit_idx / 8;    // 向下取整用于索引数组下标
   uint32_t bit_odd  = bit_idx % 8;    // 取余用于索引数组内的位

   if (value) {		  // 如果value为1
      btmp->bits[byte_idx] |= (BITMAP_MASK << bit_odd);
   } 
   else {		      // 若为0
      btmp->bits[byte_idx] &= ~(BITMAP_MASK << bit_odd);
	  /* 一般都会用个0x1这样的数对字节中的位操作,
	   * 将1任意移动后再取反,或者先取反再移位,可用来对位置0操作。*/
   }
}

