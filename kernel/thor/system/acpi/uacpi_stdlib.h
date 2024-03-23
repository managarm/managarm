#pragma once

#include <stddef.h>
#include <inttypes.h>
#include <klibc/string.h>
#include <klibc/stdlib.h>

#define UACPI_PRIx64 PRIx64
#define UACPI_PRIX64 PRIX64
#define UACPI_PRIu64 PRIu64

#define uacpi_memcpy memcpy
#define uacpi_memset memset
#define uacpi_memcmp memcmp
#define uacpi_strncmp strncmp
#define uacpi_strcmp strcmp
#define uacpi_memmove memmove
#define uacpi_strnlen strnlen
#define uacpi_strlen strlen
#define uacpi_snprintf snprintf

#define uacpi_offsetof offsetof
