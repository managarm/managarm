
void *operator new (size_t size, void *pointer);

namespace frigg {
namespace util {

// note: this class has to be placed in zero'd memory
// (e.g. in the BSS segment), otherise p_initialized will contain garbage
// we cannot use a ctor to initialize that field as this code is
// used in the dynamic linker and we want to avoid run-time relocations there
template<typename T>
class LazyInitializer {
public:
	template<typename... Args>
	void initialize(Args&&... args) {
		ASSERT(!p_initialized);
		new(p_object) T(traits::forward<Args>(args)...);
		p_initialized = true;
	}

	T *get() {
		return reinterpret_cast<T *>(p_object);
	}

	operator bool () {
		return p_initialized;
	}

	T *operator-> () {
		ASSERT(p_initialized);
		return get();
	}
	T &operator* () {
		ASSERT(p_initialized);
		return *get();
	}

private:
	char p_object[sizeof(T)];
	bool p_initialized;
};

} } // namespace frigg::util

