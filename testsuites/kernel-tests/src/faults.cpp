#include <cassert>

#include <hel-syscalls.h>
#include <hel.h>

#include "testsuite.hpp"

void *const nonCanonicalPtr = reinterpret_cast<void *>(INT64_C(-16384));
void *const illegalPtr = reinterpret_cast<void *>(0xBAD'0000'BEEF);

DEFINE_TEST(nonCanonical, ([] {
	            HelError ret =
	                helLog(kHelLogSeverityInfo, static_cast<char *>(nonCanonicalPtr), 4096);
	            assert(ret == kHelErrFault);
            }))

DEFINE_TEST(helLog_fault, ([] {
	            HelError ret = helLog(kHelLogSeverityInfo, static_cast<char *>(illegalPtr), 4096);
	            assert(ret == kHelErrFault);
            }))

DEFINE_TEST(helGetCredentials_fault, ([] {
	            HelError ret =
	                helGetCredentials(kHelThisThread, 0, static_cast<char *>(illegalPtr));
	            assert(ret == kHelErrFault);
            }))
