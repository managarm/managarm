
namespace frigg {
namespace atomic {

template<typename T, typename = void>
struct Operations {

};

template<typename T>
inline void volatileWrite(T *pointer, T value) {
	*const_cast<volatile T *>(pointer) = value;
}
template<typename T>
inline T volatileRead(T *pointer) {
	return *const_cast<volatile T *>(pointer);
}

inline void pause() {
	asm volatile ( "pause" );
}

} } // namespace frigg::atomic

