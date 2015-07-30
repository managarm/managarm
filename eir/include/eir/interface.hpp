
typedef uint64_t EirPtr;
typedef uint64_t EirSize;

struct EirModule {
	EirPtr physicalBase;
	EirSize length;
	EirPtr namePtr;
	EirSize nameLength;
};

struct EirInfo {
	EirPtr bootstrapPhysical;
	EirSize bootstrapLength;
	
	EirSize numModules;
	EirPtr moduleInfo;
};

