
static_assert(sizeof(int) == 4, "Unexpected architecture");
static_assert(sizeof(long) == 8, "Unexpected architecture");
typedef int int32_t;
typedef long int64_t;

typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

static_assert(sizeof(void *) == 8, "Unexpected architecture");
typedef uint64_t uintptr_t;

typedef uint64_t size_t;

