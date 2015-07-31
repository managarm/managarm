
void *operator new (size_t size, void *pointer);
void *operator new[] (size_t size, void *pointer);

namespace frigg {
namespace util {

template<typename T>
class LazyInitializer {
public:
	template<typename... Args>
	void initialize(Args&&... args) {
		new(p_object) T(traits::forward<Args>(args)...);
	}

	T *get() {
		return (T *)p_object;
	}
	T *operator-> () {
		return get();
	}
	T &operator* () {
		return *get();
	}

private:
	char p_object[sizeof(T)];
};

} } // namespace frigg::util

