
namespace thor {

void initializeTheSystem();

void controlArch(int interface, const void *input, void *output);

extern "C" void thorRtHalt() __attribute__ (( noreturn ));

extern "C" void thorRtReturnSyscall1(Word out0);
extern "C" void thorRtReturnSyscall2(Word out0, Word out1);
extern "C" void thorRtReturnSyscall3(Word out0, Word out1, Word out2);

} // namespace thor

