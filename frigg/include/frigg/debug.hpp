
namespace frigg {

#define assert(c) do { if(!(c)) ::frigg::assertionFail(#c, __func__, __FILE__, __LINE__); } while(0)

class VirtualSink {
public:
	virtual void print(char c) = 0;
	virtual void print(const char *str) = 0;
};

// --------------------------------------------------------
// Logging
// --------------------------------------------------------

template<typename P, typename T>
void printUInt(P &printer, T number, int radix) {
	if(number == 0) {
		printer.print('0');
		return;
	}
	const char *digits = "0123456789abcdef";
	int logarithm = 0;
	T rem = number;
	while(1) {
		rem /= radix;
		if(rem == 0)
			break;
		logarithm++;
	}
	T p = 1;
	for(int i = 0; i < logarithm; i++)
		p *= radix;
	while(p > 0) {
		int d = number / p;
		printer.print(digits[d]);
		number %= p;
		p /= radix;
	}
}


class EndLog { };

template<typename P, typename T, typename E = void>
struct Print;

template<typename P>
struct Print<P, EndLog> {
	static void print(P &printer, EndLog token) {
		printer.finish();
	}
};

template<typename P>
struct Print<P, const char *> {
	static void print(P &printer, const char *string) {
		printer.print(string);
	}
};

template<typename P, typename IntType>
struct Print<P, IntType, typename traits::EnableIf<traits::IsIntegral<IntType>::value
			&& traits::IsSigned<IntType>::value>::type> {
	static void print(P &printer, IntType number) {
		if(number < 0) {
			printer.print('-');
			printUInt(printer, -number, 10);
		}else{
			printUInt(printer, number, 10);
		}
	}
};

template<typename P, typename UIntType>
struct Print<P, UIntType, typename traits::EnableIf<traits::IsIntegral<UIntType>::value
				&& traits::IsUnsigned<UIntType>::value>::type> {
	static void print(P &printer, UIntType number) {
		printUInt(printer, number, 10);
	}
};

template<typename P, typename T>
struct Print<P, T *> {
	static void print(P &printer, T *pointer) {
		printer.print("0x");
		printUInt(printer, (uintptr_t)pointer, 16);
	}
};

template<typename P, typename T,
		typename E = typename P::IsPrinter>
P operator<< (P &&printer, T object) {
	Print<P, T>::print(printer, object);
	return printer;
}

template<typename P, typename T,
		typename E = typename P::IsPrinter>
P &operator<< (P &printer, T object) {
	Print<P, T>::print(printer, object);
	return printer;
}

template<typename Sink>
class DefaultLogger {
public:
	class Printer {
	public:
		struct IsPrinter { };

		Printer(Sink &sink) : p_sink(sink) { }

		void print(char c) {
			p_sink.print(c);
		}
		void print(const char *str) {
			p_sink.print(str);
		}

		void finish() {
			p_sink.print('\n');
		}

	private:
		Sink &p_sink;
	};

	DefaultLogger(Sink &sink) : p_sink(sink) { }

	Printer log() {
		return Printer(p_sink);
	}

private:
	Sink &p_sink;
};

class PanicLogger {
public:
	class Printer {
	public:
		struct IsPrinter { };

		void print(char c);
		void print(const char *str);

		void finish();
	};

	Printer log();
};

extern PanicLogger panicLogger;

// --------------------------------------------------------
// Namespace scope functions
// --------------------------------------------------------

void assertionFail(const char *message, const char *function,
		const char *file, int line);

} // namespace frigg

