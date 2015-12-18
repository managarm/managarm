
#ifndef FRIGG_PRINTF_HPP
#define FRIGG_PRINTF_HPP

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

		assert(!left_justify);
		assert(!always_sign);
		assert(!plus_becomes_space);
		assert(!alt_conversion);
		assert(!fill_zeros);

		int minimum_width = 0;
		while(*format >= '0' && *format <= '9') {
			minimum_width = minimum_width * 10 + (*format - '0');
			++format;
			assert(*format);
		}

		assert(minimum_width == 0);
		
		bool has_precision = false;
		int precision = 0;
		if(*format == '.') {
			++format;
			assert(*format);

			has_precision = true;
			
			if(*format == '*') {
				++format;
				assert(*format);
				precision = va_arg(args, int);
			}else{
				assert(*format >= '0' && *format <= '9');
				while(*format >= '0' && *format <= '9') {
					precision = precision * 10 + (*format - '0');
					++format;
					assert(*format);
				}
			}
		}
		
		bool l_prefix = false;
		if(*format == 'l') {
			l_prefix = true;
			++format;
		}

		switch(*format) {
		case '%':
			printer.print('%');
			break;
		case 'c':
			assert(!l_prefix);
			assert(precision == 0);
			printer.print((char)va_arg(args, int));
			break;
		case 's':
			assert(!l_prefix);
			if(has_precision) {
				const char *s = va_arg(args, const char *);
				for(int i = 0; i < precision && s[i]; i++)
					printer.print(s[i]);
			}else{
				printer.print(va_arg(args, const char *));
			}
			break;
		case 'd':
		case 'i': {
			assert(!l_prefix);
			assert(precision == 0);
			int number = va_arg(args, int);
			if(number < 0) {
				printer.print('-');
				printUInt(printer, -number, 10);
			}else{
				printUInt(printer, number, 10);
			}
		} break;
		case 'o': assert(!"Not implemented");
		case 'x':
			// TODO: Implement this correctly
			assert(!l_prefix);
			assert(precision == 0);
			printUInt(printer, va_arg(args, unsigned long), 10);
			break;
		case 'X':
			assert(!l_prefix);
			assert(precision == 0);
			printUInt(printer, va_arg(args, unsigned int), 16);
			break;
		case 'u': {
			assert(precision == 0);
			if(l_prefix) {
				printUInt(printer, va_arg(args, unsigned long), 10);
			}else{
				printUInt(printer, va_arg(args, unsigned int), 10);
			}
		} break;
		default:
			assert(!"Illegal printf modifier");
		}
		++format;
	}
}

} // namespace frigg

#endif // FRIGG_PRINTF_HPP

