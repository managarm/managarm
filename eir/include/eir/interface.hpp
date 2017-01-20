
static const uint64_t eirSignatureValue = 0x68692C2074686F72;

typedef uint64_t EirPtr;
typedef uint64_t EirSize;

struct EirRegion {
	EirPtr address;
	EirSize length;
	EirSize order; // TODO: This could be an int.
	EirSize numRoots;
};

struct EirModule {
	EirPtr physicalBase;
	EirSize length;
	EirPtr namePtr;
	EirSize nameLength;
};

struct EirInfo {
	uint64_t signature;
	EirRegion coreRegion;

	EirSize numModules;
	EirPtr moduleInfo;
};

