
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
		assert(*format != 0);
		
		bool left_justify = false;
		bool always_sign = false;
		bool plus_becomes_space = false;
		bool alt_conversion = false;
		bool fill_zeros = false;

		while(true) {
			if(*format == '-') {
				left_justify = true;
				++format;
				assert(*format != 0);
			}else if(*format == '+') {
				always_sign = true;
				++format;
				assert(*format != 0);
			}else if(*format == ' ') {
				plus_becomes_space = true;
				++format;
				assert(*format != 0);
			}else if(*format == '#') {
				alt_conversion = true;
				++format;
				assert(*format != 0);
			}else if(*format == '0') {
				fill_zeros = true;
				++format;
				assert(*format != 0);
			}else{
				break;
			}
		}

		int minimum_width = 0;
		while(*format >= '0' && *format <= '9') {
			minimum_width = minimum_width * 10 + (*format - '0');
			++format;
			assert(*format != 0);
		}
		
		int precision = 0;
		if(*format == '.') {
			++format;
			assert(*format != 0);

			while(*format >= '0' && *format <= '9') {
				precision = precision * 10 + (*format - '0');
				++format;
				assert(*format != 0);
			}
		}
		
		bool l_prefix = false;
		if(*format == 'l') {
			l_prefix = true;
			++format;
		}

		switch(*format) {
		case 'u': {
			if(l_prefix) {
				printUInt(printer, va_arg(args, unsigned long), 10);
			}else{
				printUInt(printer, va_arg(args, unsigned int), 10);
			}
		} break;
		case 'd': {
			int number = va_arg(args, int);
			if(number < 0) {
				printer.print('-');
				printUInt(printer, -number, 10);
			}else{
				printUInt(printer, number, 10);
			}
		} break;
		case 's':
			printer.print(va_arg(args, const char *));
			break;
		default:
			assert(!"Illegal printf modifier");
		}
		++format;
	}
}

} // namespace frigg

#endif // FRIGG_PRINTF_HPP

