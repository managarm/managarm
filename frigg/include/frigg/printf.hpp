
#ifndef FRIGG_PRINTF_HPP
#define FRIGG_PRINTF_HPP

#include <frigg/optional.hpp>

namespace frigg {

template<typename P>
void printf(P &printer, const char *format, va_list args) {
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
		assert(!alt_conversion);

		int minimum_width = 0;
		while(*format >= '0' && *format <= '9') {
			minimum_width = minimum_width * 10 + (*format - '0');
			++format;
			assert(*format);
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
		
		bool l_prefix = false;
		if(*format == 'l') {
			l_prefix = true;
			++format;
		}

		switch(*format) {
		case '%':
			assert(!fill_zeros);
			assert(!left_justify);
			assert(minimum_width == 0);
			printer.print('%');
			break;
		case 'c':
			assert(!fill_zeros);
			assert(!left_justify);
			assert(minimum_width == 0);
			assert(!l_prefix);
			assert(!precision);
			printer.print((char)va_arg(args, int));
			break;
		case 's': {
			assert(!fill_zeros);
			assert(!l_prefix);

			auto s = va_arg(args, const char *);
			int length = strlen(s);
			if(precision && *precision < length)
				length = *precision;

			if(left_justify) {
				for(int i = 0; i < *precision && s[i]; i++)
					printer.print(s[i]);
				for(int i = length; i < minimum_width; i++)
					printer.print(' ');
			}else{
				for(int i = 0; i < *precision && s[i]; i++)
					printer.print(s[i]);
				for(int i = length; i < minimum_width; i++)
					printer.print(' ');
			}
		} break;
		case 'd':
		case 'i': {
			assert(!fill_zeros);
			assert(!left_justify);
			assert(minimum_width == 0);
			assert(!l_prefix);
			int number = va_arg(args, int);
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
		case 'o': assert(!"Not implemented");
		case 'x':
			// TODO: Implement this correctly
			assert(!fill_zeros);
			assert(!left_justify);
			assert(minimum_width == 0);
			assert(!l_prefix);
			assert(!precision);
			printUInt(printer, va_arg(args, unsigned int), 16);
			break;
		case 'X': {
			assert(!left_justify);
			assert(!l_prefix);
			auto number = va_arg(args, unsigned int);
			if(precision && *precision == 0 && !number) {
				// print nothing in this case
			}else{
				printUInt(printer, number, 16, minimum_width,
						precision ? *precision : 1, fill_zeros ? '0' : ' ');
			}
		} break;
		case 'u': {
			assert(!fill_zeros);
			assert(!left_justify);
			assert(minimum_width == 0);
			assert(!precision);
			if(l_prefix) {
				printUInt(printer, va_arg(args, unsigned long), 10);
			}else{
				printUInt(printer, va_arg(args, unsigned int), 10);
			}
		} break;
		case 'p':
			assert(!fill_zeros);
			assert(!left_justify);
			assert(minimum_width == 0);
			printer.print("0x");
			printUInt(printer, (uintptr_t)va_arg(args, void *), 16);
			break;
		default:
			assert(!"Illegal printf modifier");
		}
		++format;
	}
}

} // namespace frigg

#endif // FRIGG_PRINTF_HPP

