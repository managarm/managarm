
namespace frigg {
namespace traits {

template<typename... Types>
struct TypePack;

template<bool condition, typename T = void>
struct EnableIf;

template<typename T>
struct EnableIf<true, T> {
	typedef T type;
};

template<typename T>
struct IsIntegral {
	static constexpr bool value = false;
};

template<> struct IsIntegral<char> { static constexpr bool value = true; };
template<> struct IsIntegral<int> { static constexpr bool value = true; };
template<> struct IsIntegral<short> { static constexpr bool value = true; };
template<> struct IsIntegral<long> { static constexpr bool value = true; };
template<> struct IsIntegral<long long> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned char> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned int> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned short> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned long> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned long long> { static constexpr bool value = true; };

template<typename T>
struct IsSigned {
	static constexpr bool value = false;
};

template<> struct IsSigned<char> { static constexpr bool value = true; };
template<> struct IsSigned<int> { static constexpr bool value = true; };
template<> struct IsSigned<short> { static constexpr bool value = true; };
template<> struct IsSigned<long> { static constexpr bool value = true; };
template<> struct IsSigned<long long> { static constexpr bool value = true; };

template<typename T>
struct IsUnsigned {
	static constexpr bool value = false;
};

template<> struct IsUnsigned<unsigned char> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned int> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned short> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned long> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned long long> { static constexpr bool value = true; };

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
typename RemoveRef<T>::type &&move(T &&arg) noexcept {
	return static_cast<typename RemoveRef<T>::type &&>(arg);
} 

template<typename T>
T &&forward(typename RemoveRef<T>::type &arg) noexcept {
	return static_cast<T &&>(arg);
} 

}} // namespace frigg::traits
