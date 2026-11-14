#include <frg/bitops.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kcov.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

std::expected<smarter::shared_ptr<KcovMemory>, Error> KcovMemory::create(size_t length) {
	if(!length || length % sizeof(uint64_t) || length > maxSize)
		return std::unexpected{Error::illegalArgs};

	auto memoryOutcome = ImmediateMemory::create(frg::align_up(length, kPageSize));
	if(!memoryOutcome)
		return std::unexpected{memoryOutcome.error()};

	return allocate_rcu_shared<KcovMemory>(*kernelAlloc, CtorToken{},
			std::move(*memoryOutcome), length / sizeof(uint64_t));
}

KcovMemory::KcovMemory(CtorToken, smarter::shared_ptr<ImmediateMemory> memory, size_t numWords)
: _memory{std::move(memory)}, _window{_memory},
		_area{reinterpret_cast<uint64_t *>(_window.bytes_data())},
		_numWords{numWords} { }

size_t KcovMemory::getLength() {
	return _memory->getLength();
}

Error KcovMemory::lockRange(uintptr_t offset, size_t size) {
	return _memory->lockRange(offset, size);
}

void KcovMemory::unlockRange(uintptr_t offset, size_t size) {
	_memory->unlockRange(offset, size);
}

PhysicalRange KcovMemory::peekRange(uintptr_t offset, FetchFlags flags) {
	return _memory->peekRange(offset, flags);
}

coroutine<frg::expected<Error, size_t>>
KcovMemory::touchRange(uintptr_t offset, size_t sizeHint, FetchFlags flags) {
	co_return co_await _memory->touchRange(offset, sizeHint, flags);
}

std::expected<size_t, Error> KcovMemory::accessRange(uintptr_t offset, size_t size,
		FetchFlags flags, PageAccessFn fn) {
	return _memory->accessRange(offset, size, flags, fn);
}

// --------------------------------------------------------
// Buffer handling
// --------------------------------------------------------

Error KcovMemory::attach(smarter::shared_ptr<KcovMemory> memory, Thread *thread, unsigned int mode) {
	if(mode != kcovModePc && mode != kcovModeCmp)
		return Error::illegalArgs;

	auto kcov = memory.get();
	unsigned int expected = kcovModeDisabled;
	if(!__atomic_compare_exchange_n(&kcov->mode, &expected, mode,
			false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		return Error::illegalState;

	smarter::shared_ptr<KcovMemory> previous;
	{
		StatelessIrqLock irqLock;
		auto lock = frg::guard(&thread->kcovMutex);

		if(thread->kcovExited || thread->kcov) {
			__atomic_store_n(&kcov->mode, kcovModeDisabled, __ATOMIC_RELAXED);
			return thread->kcovExited ? Error::threadExited : Error::illegalState;
		}

		previous = std::move(thread->kcovRef);
		thread->kcovRef = std::move(memory);
		__atomic_store_n(&thread->kcov, kcov, __ATOMIC_RELEASE);
	}
	return Error::success;
}

Error KcovMemory::detach(KcovMemory *memory, Thread *thread) {
	StatelessIrqLock irqLock;
	auto lock = frg::guard(&thread->kcovMutex);

	if(thread->kcov != memory)
		return Error::illegalState;

	__atomic_store_n(&thread->kcov, nullptr, __ATOMIC_RELAXED);
	__atomic_store_n(&memory->mode, kcovModeDisabled, __ATOMIC_RELAXED);
	return Error::success;
}

void KcovMemory::exitCurrent(Thread *thread) {
	smarter::shared_ptr<KcovMemory> ref;
	{
		StatelessIrqLock irqLock;
		auto lock = frg::guard(&thread->kcovMutex);

		thread->kcovExited = true;
		if(auto kcov = thread->kcov; kcov) {
			__atomic_store_n(&thread->kcov, nullptr, __ATOMIC_RELAXED);
			__atomic_store_n(&kcov->mode, kcovModeDisabled, __ATOMIC_RELAXED);
		}
		ref = std::move(thread->kcovRef);
	}
}

// --------------------------------------------------------
// Coverage hooks
// --------------------------------------------------------

namespace {

inline constexpr uint64_t kcovCmpFlagsConst = 1;
inline constexpr uint64_t kcovCmpFlags8Bit = 0;
inline constexpr uint64_t kcovCmpFlags16Bit = 2;
inline constexpr uint64_t kcovCmpFlags32Bit = 4;
inline constexpr uint64_t kcovCmpFlags64Bit = 6;

THOR_KCOV_DISABLED KcovMemory *currentKcov(unsigned int mode) {
	if(!cpuDataAvailable)
		return nullptr;
	if(contextIpl() > ipl::exceptional)
		return nullptr;
	auto thread = getCpuData()->activeThread;
	if(!thread)
		return nullptr;
	auto kcov = __atomic_load_n(&thread->kcov, __ATOMIC_ACQUIRE);
	if(!kcov || __atomic_load_n(&kcov->mode, __ATOMIC_RELAXED) != mode)
		return nullptr;
	return kcov;
}

THOR_KCOV_DISABLED void publishPc(uint64_t pc) {
	auto kcov = currentKcov(kcovModePc);
	if(!kcov)
		return;
	kcov->recordPc(pc);
}

THOR_KCOV_DISABLED void publishCmp(uint64_t pc, uint64_t v1, uint64_t v2, uint64_t flags) {
	auto kcov = currentKcov(kcovModeCmp);
	if(!kcov)
		return;
	kcov->recordCmp(pc, v1, v2, flags);
}

} // anonymous namespace

THOR_KCOV_DISABLED size_t KcovMemory::_allocateRecord(size_t recordWords) {
	auto maxRecords = (_numWords - 1) / recordWords;
	auto count = __atomic_load_n(&_area[0], __ATOMIC_RELAXED);
	while(count < maxRecords) {
		if(__atomic_compare_exchange_n(&_area[0], &count, count + 1,
				true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			return 1 + count * recordWords;
	}
	return 0;
}

THOR_KCOV_DISABLED void KcovMemory::recordPc(uint64_t pc) {
	auto index = _allocateRecord(1);
	if(!index)
		return;
	_area[index] = pc;
}

THOR_KCOV_DISABLED void KcovMemory::recordCmp(uint64_t pc, uint64_t v1, uint64_t v2, uint64_t flags) {
	auto index = _allocateRecord(4);
	if(!index)
		return;
	_area[index] = flags;
	_area[index + 1] = v1;
	_area[index + 2] = v2;
	_area[index + 3] = pc;
}

} // namespace thor

using namespace thor;

// The coverage hooks use noinline so that they can properly see the return PC with LTO

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_pc() {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishPc(pc);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp1(uint8_t a1, uint8_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags8Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp2(uint16_t a1, uint16_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags16Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp4(uint32_t a1, uint32_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags32Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_const_cmp8(uint64_t a1, uint64_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlagsConst | kcovCmpFlags64Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp1(uint8_t a1, uint8_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags8Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp2(uint16_t a1, uint16_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags16Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp4(uint32_t a1, uint32_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags32Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_cmp8(uint64_t a1, uint64_t a2) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	publishCmp(pc, a1, a2, kcovCmpFlags64Bit);
}

extern "C" [[gnu::noinline]] THOR_KCOV_DISABLED void __sanitizer_cov_trace_switch(uint64_t v, uint64_t *cases) {
	uint64_t pc = (uint64_t)__builtin_return_address(0);
	uint64_t flags = kcovCmpFlagsConst;
	switch(cases[1]) {
	case 8:
		flags |= kcovCmpFlags8Bit;
		break;
	case 16:
		flags |= kcovCmpFlags16Bit;
		break;
	case 32:
		flags |= kcovCmpFlags32Bit;
		break;
	case 64:
		flags |= kcovCmpFlags64Bit;
		break;
	default:
		return;
	}

	for(uint64_t i = 0; i < cases[0]; ++i)
		publishCmp(pc, cases[i + 2], v, flags);
}
