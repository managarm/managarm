
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <hel.h>
#include <hel-syscalls.h>

/*
void secondThread(uintptr_t argument) {
	helExitThisThread();
}
*/

//extern "C" void putchar(char c);

int main() {
/*
	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };

	HelHandle io_space;
	helAccessIo(ports, 9, &io_space);
*/
//	if(stdout == NULL)
//		helLog("wtf", 3);
//	putc('x', stdout);

	/*char *x = (char *)malloc(2);
	x[0] = 'h';
	x[1] = 'i';
	helLog(x, 2);*/

	//putchar('x');
	printf("Hello World!");
//	exit(0);

/*	HelHandle memory;
	helAllocateMemory(0x1000, &memory);
	helMapMemory(memory, (void *)0x2001000, 0x1000);

	HelHandle thread;
	helCreateThread(&secondThread, 0, (void *)0x2002000, &thread);*/

//	const char *hello = "hello";

/*	HelHandle first, second;
	helCreateBiDirectionPipe(&first, &second);
	
	char buffer[5];
	helSendString(first, hello, 5);
	helRecvString(second, buffer, 5);
	helLog(buffer, 5);

	helLog("ok", 2);*/
}

