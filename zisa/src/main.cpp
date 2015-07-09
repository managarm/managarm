
#include <stdlib.h>
#include <stdint.h>
#include <hel.h>
#include <hel-syscalls.h>

void secondThread(uintptr_t argument) {
	helExitThisThread();
}

int main() {
	HelHandle memory;
	helAllocateMemory(0x1000, &memory);
	helMapMemory(memory, (void *)0x2001000, 0x1000);

	HelHandle thread;
	helCreateThread(&secondThread, 0, (void *)0x2002000, &thread);

//	const char *hello = "hello";

/*	HelHandle first, second;
	helCreateBiDirectionPipe(&first, &second);
	
	char buffer[5];
	helSendString(first, hello, 5);
	helRecvString(second, buffer, 5);
	helLog(buffer, 5);

	helLog("ok", 2);*/
}

