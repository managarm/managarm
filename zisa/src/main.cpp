
#include "../../frigg/include/types.hpp"
#include "../../hel/include/hel.h"

int main() {
	const char *hello = "hello";

	HelHandle first, second;
	helCreateBiDirectionPipe(&first, &second);
	
	char buffer[5];
	helSendString(first, hello, 5);
	helRecvString(second, buffer, 5);
	helLog(buffer, 5);

	helLog("ok", 2);
}

