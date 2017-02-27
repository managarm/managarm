#ifndef THOR_LIBC_CTYPE_H
#define THOR_LIBC_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

// Character classification function [7.4.1]
int isalpha(int c);
int isdigit(int c);
int isprint(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);

// Character case mapping functions [7.4.2]
int tolower(int c);
int toupper(int c);

#ifdef __cplusplus
}
#endif

#endif // THOR_LIBC_CTYPE_H
