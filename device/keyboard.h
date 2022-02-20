#ifndef __DEVICE_KEYBOARD_H
#define __DEVICE_KEYBOARD_H
void keyboard_init(void); 

extern struct ioqueue kbd_buf;
/*	###struct ioqueue kbd_buf定義在keyboard.c中，
	###所以在此加入extern，
	###
	###在此寫extern struct ioqueue kbd_buf，
	###這樣main.c include keyboard.h進來後，
	###也能用此一模一樣kbd_buf。	*/

#endif
