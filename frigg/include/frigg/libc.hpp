
extern "C" int isdigit(int ch);
extern "C" int isspace(int ch);
extern "C" int isprint(int ch);
extern "C" int isxdigit(int ch);

extern "C" int toupper(int ch);
extern "C" int tolower(int ch);

extern "C" int memcmp(const void *lhs, const void *rhs, size_t count);
extern "C" void *memcpy(void *dest, const void *src, size_t n);
extern "C" void *memset(void *dest, int byte, size_t count);

extern "C" unsigned long strtoul(const char *str, char **str_end, int base);
extern "C" size_t strlen(const char *str);
extern "C" int strcmp(const char *lhs, const char *rhs);
extern "C" int strncmp(const char *lhs, const char *rhs, size_t count);
extern "C" char *strcpy(char *dest, const char *src);
extern "C" char *strncpy(char *dest, const char *src, size_t count);
extern "C" char *strcat(char *dest, const char *src);

