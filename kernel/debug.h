#ifndef __KERNEL_DEBUG_H
	#define __KERNEL_DEBUG_H
	void panic_spin(char* filename, int line, const char* func, const char* condition);
	
/***************************  __VA_ARGS__  *******************************
* __VA_ARGS__ 是预处理器所支持的专用标识符。
* 代表所有与省略号相对应的参数. 
* "..."表示定义的宏其参数可变.*/
	#define PANIC(...) panic_spin (__FILE__, __LINE__, __func__, __VA_ARGS__)
/***********************************************************************/
	
	#ifdef NDEBUG
		#define ASSERT(CONDITION) ((void)0)
		//把ASSERT(CONDITION)定義成0，再強制轉換成void，形同什麼都沒有
	#else
		#define ASSERT(CONDITION)                       	\
			if (CONDITION) {                                \
			} 												\
			else {                      					\
			/* 符号#让编译器将宏的参数转化为字符串字面量，  
			   比如說1 == 2是一個判斷式，                  
			   前面加一個讓編譯器把判斷式轉換成一整個字串，
			   即變成"1 == 2"							*/	\
				PANIC(#CONDITION);                          \
			}
	#endif /*__NDEBUG */

#endif /*__KERNEL_DEBUG_H*/

/*	###假如在main.c寫ASSERT(1 == 2)，
	###進入標頭檔後，ASSERT(1 == 2)會變成ASSERT(CONDITION)，其中CONDITION等於1 == 2，
	###如果debug.h內有define NDEBUG，會把ASSERT(CONDITION)設成空0，相當於刪除，
	###如果沒有define NDEBUG，則考慮兩種情況:
	###ASSERT(CONDITION)的CONDITION為真，則進入空大括號，表示什麼也不做，
	###ASSERT(CONDITION)的CONDITION為假，則ASSER(CONDITION) define成 PANIC(#CONDITION)，
	###PANIC(#CONDITION)因為第9行的定義變成PANIC(...)，
	###然後PANIC(...)變成panic_spin (__FILE__, __LINE__, __func__, __VA_ARGS__)，
	###其中__VA_ARGS__代表"..."，即轉化成字串的"1 == 2"，
	###panic_spin (__FILE__, __LINE__, __func__, __VA_ARGS__)一定義出來，
	###main.c的ASSERT(1 == 2)會瞬間變成panic_spin (檔案名, 行號, 函數名(此處為main), "1 == 2")。
	###然後進入void panic_spin(char* filename, int line, const char* func, const char* condition)顯示錯誤訊息。
*/