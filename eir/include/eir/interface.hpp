
typedef uint64_t EirPtr;
typedef uint64_t EirSize;

struct EirModule {
	EirPtr physicalBase;
	EirSize length;
	EirPtr namePtr;
	EirSize nameLength;
};

struct EirInfo {
	EirPtr address;
	EirSize length;
	EirSize order; // TODO: This could be an int.
	EirSize numRoots;

	EirSize numModules;
	EirPtr moduleInfo;
};

