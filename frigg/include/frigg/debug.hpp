
#ifndef FRIGG_DEBUG_HPP
#define FRIGG_DEBUG_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>

namespace frigg FRIGG_VISIBILITY {

class VirtualSink {
public:
	virtual void print(char c) = 0;
	virtual void print(const char *str) = 0;
};

// --------------------------------------------------------
// Logging
// --------------------------------------------------------

// width: Minimum width of the output (padded with spaces by default).
// precision: Minimum number of digits in the ouput (always padded with zeros).
template<typename P, typename T>
void printUInt(P &printer, T number, int radix, int width = 0,
		int precision = 1, char padding = ' ') {
	const char *digits = "0123456789abcdef";

	// print the number in reverse order and determine #digits.
	char buffer[32];
	int k = 0; // number of digits
	do {
		assert(k < 32); // TODO: variable number of digits
		buffer[k++] = digits[number % radix];
		number /= radix;
	} while(number);

	if(max(k, precision) < width)
		for(int i = 0; i < width - max(k, precision); i++)
			printer.print(padding);
	if(k < precision)
		for(int i = 0; i < precision - k; i++)
			printer.print('0');
	for(int i = k - 1; i >= 0; i--)
		printer.print(buffer[i]);
}


class EndLog { };
static constexpr EndLog endLog;

template<typename T>
struct LogHex {
	LogHex(const T &value)
	: value(value) { }

	const T value;
};

template<typename T>
LogHex<T> logHex(const T &value) {
	return LogHex<T>(value);
}

template<typename P, typename T, typename E = void>
struct Print;

template<typename P>
struct Print<P, EndLog> {
	static void print(P &printer, EndLog) {
		printer.finish();
	}
};

template<typename P, typename T>
struct Print<P, LogHex<T>> {
	static void print(P &printer, LogHex<T> token) {
		printUInt(printer, token.value, 16);
	}
};

template<typename P>
struct Print<P, const char *> {
	static void print(P &printer, const char *string) {
		printer.print(string);
	}
};

template<typename P, typename IntType>
struct Print<P, IntType, typename EnableIf<IsIntegral<IntType>::value
			&& IsSigned<IntType>::value>::type> {
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
struct Print<P, UIntType, typename EnableIf<IsIntegral<UIntType>::value
				&& IsUnsigned<UIntType>::value>::type> {
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

class InfoLogger {
public:
	class Printer {
	public:
		struct IsPrinter { };

		void print(char c);
		void print(const char *str);

		void finish();
	};

	Printer operator() ();
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

	Printer operator() ();
};

extern InfoLogger infoLogger;
extern PanicLogger panicLogger;

} // namespace frigg

#endif // FRIGG_DEBUG_HPP

