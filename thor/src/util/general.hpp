
namespace thor {
namespace util {

template<typename T>
struct RemoveRef {
	typedef T type;
};
template<typename T>
struct RemoveRef<T &> {
	typedef T type;
};
template<typename T>
struct RemoveRef<T &&> {
	typedef T type;
};

template<typename T> 
typename RemoveRef<T>::type && move(T &&arg) noexcept {
	return static_cast<typename RemoveRef<T>::type &&>(arg);
} 

template<typename T>
T &&forward(typename RemoveRef<T>::type &arg) noexcept {
	return static_cast<T &&>(arg);
} 

}} // namespace thor::util

