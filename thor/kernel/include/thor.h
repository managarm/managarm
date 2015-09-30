
enum {
	kThorSubGeneric = 1,
	kThorSubArch = 2,
	kThorSubDebug = 3
};

enum {
	// x86_64 architecture interfaces
	kThorIfSetupHpet = 1,
	kThorIfSetupIoApic = 2,
	kThorIfBootSecondary = 3,
	kThorIfFinishBoot = 4,

	// debug interfaces
	kThorIfDebugMemory = 1
};

