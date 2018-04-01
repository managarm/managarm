
#ifndef FRIGG_PRINTF_HPP
#define FRIGG_PRINTF_HPP

#include <stdarg.h>
#include <frigg/macros.hpp>
#include <frigg/optional.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename P>
void printf(P &printer, const char *format, va_list args) {
	enum class SizeMod {
		defaultSize,
		longSize,
		longLongSize,
		nativeSize
	};

	while(*format != 0) {
		if(*format != '%') {
			printer.print(*format);
			if(*format == '\n')
				printer.flush();
			++format;
			continue;
		}

		++format;
		assert(*format);
		
		bool left_justify = false;
		bool always_sign = false;
		bool plus_becomes_space = false;
		bool alt_conversion = false;
		bool fill_zeros = false;

		while(true) {
			if(*format == '-') {
				left_justify = true;
				++format;
				assert(*format);
			}else if(*format == '+') {
				always_sign = true;
				++format;
				assert(*format);
			}else if(*format == ' ') {
				plus_becomes_space = true;
				++format;
				assert(*format);
			}else if(*format == '#') {
				alt_conversion = true;
				++format;
				assert(*format);
			}else if(*format == '0') {
				fill_zeros = true;
				++format;
				assert(*format);
			}else{
				break;
			}
		}

		assert(!always_sign);
		assert(!plus_becomes_space);

		int minimum_width = 0;
		if(*format == '*') {
			++format;
			assert(*format);
			minimum_width = va_arg(args, int);
		}else{
			while(*format >= '0' && *format <= '9') {
				minimum_width = minimum_width * 10 + (*format - '0');
				++format;
				assert(*format);
			}
		}

		Optional<int> precision;
		if(*format == '.') {
			++format;
			assert(*format);

			if(*format == '*') {
				++format;
				assert(*format);
				precision = va_arg(args, int);
			}else{
				int value = 0;
				assert(*format >= '0' && *format <= '9');
				while(*format >= '0' && *format <= '9') {
					value = value * 10 + (*format - '0');
					++format;
					assert(*format);
				}
				precision = value;
			}
		}
		
		SizeMod szmod = SizeMod::defaultSize;
		if(*format == 'l') {
			++format;
			if(*format == 'l') {
				++format;
				szmod = SizeMod::longLongSize;
			}else{
				szmod = SizeMod::longSize;
			}
		}else if(*format == 'z') {
			++format;
			szmod = SizeMod::nativeSize;
		}

		switch(*format) {
		case '%':
			assert(!fill_zeros);
			assert(!left_justify);
			assert(!alt_conversion);
			assert(minimum_width == 0);
			printer.print('%');
			break;
		case 'c':
			assert(!fill_zeros);
			assert(!left_justify);
			assert(!alt_conversion);
			assert(minimum_width == 0);
			assert(szmod == SizeMod::defaultSize);
			assert(!precision);
			printer.print((char)va_arg(args, int));
			break;
		case 's': {
			assert(!fill_zeros);
			assert(!alt_conversion);
			assert(szmod == SizeMod::defaultSize);

			auto s = va_arg(args, const char *);
			if(!s)
				s = "(null)";

			int length = strlen(s);
			if(precision && *precision < length)
				length = *precision;

			if(left_justify) {
				for(int i = 0; i < length && s[i]; i++)
					printer.print(s[i]);
				for(int i = length; i < minimum_width; i++)
					printer.print(' ');
			}else{
				for(int i = 0; i < length && s[i]; i++)
					printer.print(s[i]);
				for(int i = length; i < minimum_width; i++)
					printer.print(' ');
			}
		} break;
		case 'd':
		case 'i': {
			assert(!left_justify);
			assert(!alt_conversion);
			long number;
			if(szmod == SizeMod::longSize) {
				number = va_arg(args, long);
			}else if(szmod == SizeMod::longLongSize) {
				number = va_arg(args, long long);
			}else if(szmod == SizeMod::nativeSize) {
				number = va_arg(args, intptr_t);
			}else{
				assert(szmod == SizeMod::defaultSize);
				number = va_arg(args, int);
			}
			if(precision && *precision == 0 && !number) {
				// print nothing in this case
			}else if(number < 0) {
				// FIXME: this is horribly broken!
				printer.print('-');
				printUInt(printer, -number, 10, minimum_width,
						precision ? *precision : 1, fill_zeros ? '0' : ' ');
			}else{
				printUInt(printer, number, 10, minimum_width,
						precision ? *precision : 1, fill_zeros ? '0' : ' ');
			}
		} break;
		case 'o': {
			// TODO: Implement this correctly
			assert(!left_justify);

			auto print = [&] (auto number) {
				if(precision && *precision == 0 && !number) {
					// print nothing in this case
				}else{
					printUInt(printer, number, 8, minimum_width,
							precision ? *precision : 1, fill_zeros ? '0' : ' ');
				}
			};

			if(alt_conversion)
				printer.print('0');

			if(szmod == SizeMod::longSize) {
				print(va_arg(args, unsigned long));
			}else{
				assert(szmod == SizeMod::defaultSize);
				print(va_arg(args, unsigned int));
			}
		} break;
		case 'x': {
			// TODO: Implement this correctly
			assert(!left_justify);
			assert(!alt_conversion);
			auto print = [&] (auto number) {
				if(precision && *precision == 0 && !number) {
					// print nothing in this case
				}else{
					printUInt(printer, number, 16, minimum_width,
							precision ? *precision : 1, fill_zeros ? '0' : ' ');
				}
			};
			if(szmod == SizeMod::longSize) {
				print(va_arg(args, unsigned long));
			}else{
				assert(szmod == SizeMod::defaultSize);
				print(va_arg(args, unsigned int));
			}
		} break;
		case 'X': {
			assert(!left_justify);
			assert(!alt_conversion);
			auto print = [&] (auto number) {
				if(precision && *precision == 0 && !number) {
					// print nothing in this case
				}else{
					printUInt(printer, number, 16, minimum_width,
							precision ? *precision : 1, fill_zeros ? '0' : ' ');
				}
			};
			if(szmod == SizeMod::longSize) {
				print(va_arg(args, unsigned long));
			}else{
				print(va_arg(args, unsigned int));
			}
		} break;
		case 'u': {
			assert(!left_justify);
			assert(!alt_conversion);
			assert(!precision);
			if(szmod == SizeMod::longLongSize) {
				printUInt(printer, va_arg(args, unsigned long long), 10, minimum_width,
						1, fill_zeros ? '0' : ' ');
			}else if(szmod == SizeMod::longSize) {
				printUInt(printer, va_arg(args, unsigned long), 10, minimum_width,
						1, fill_zeros ? '0' : ' ');
			}else if(szmod == SizeMod::nativeSize) {
				printUInt(printer, va_arg(args, size_t), 10, minimum_width,
						1, fill_zeros ? '0' : ' ');
			}else{
				assert(szmod == SizeMod::defaultSize);
				printUInt(printer, va_arg(args, unsigned int), 10, minimum_width,
						1, fill_zeros ? '0' : ' ');
			}
		} break;
		case 'p':
			assert(!fill_zeros);
			assert(!left_justify);
			assert(!alt_conversion);
			assert(minimum_width == 0);
			printer.print("0x");
			printUInt(printer, (uintptr_t)va_arg(args, void *), 16);
			break;
		case 'f':
		case 'F':
		case 'g':
		case 'G':
		case 'e':
		case 'E':
		case 'm':
			printer.print("%f");
			break;
		default:
			assert(!"Illegal printf modifier");
		}
		++format;
	}
}

} // namespace frigg

#endif // FRIGG_PRINTF_HPP

