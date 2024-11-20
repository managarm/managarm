#pragma once

// based on UEFI 2.10 Errata A

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

// 2.3.1 Data Types
using efi_status = size_t;
using efi_handle = void *;

struct alignas(8) efi_guid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};

// 4.2.1 EFI_TABLE_HEADER

struct efi_table_header {
	uint64_t signature;
	uint32_t revision;
	uint32_t header_size;
	uint32_t crc32;
	uint32_t reserved;
};

// 4.3.1 EFI_SYSTEM_TABLE

struct efi_boot_services;
struct efi_configuration_table;
struct efi_simple_text_output_protocol;

struct efi_system_table {
	efi_table_header hdr;
	char16_t *firmware_vendor;
	uint32_t firmware_revision;
	efi_handle console_in_handle;
	void *con_in;
	efi_handle console_out_handle;
	struct efi_simple_text_output_protocol *con_out;
	efi_handle standard_error_handle;
	void *std_err;
	void *runtime_services;
	efi_boot_services *boot_services;
	size_t number_of_table_entries;
	efi_configuration_table *configuration_table;
};

// 4.6.1 EFI_CONFIGURATION_TABLE
struct efi_configuration_table {
	efi_guid vendor_guid;
	void *vendor_table;
};

constexpr efi_guid ACPI_20_TABLE_GUID = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };

// 7.2.1 EFI_BOOT_SERVICES.AllocatePages()
enum efi_allocate_type {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
};

enum efi_memory_type {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiUnacceptedMemoryType,
	EfiMaxMemoryType
};

using efi_physical_addr = uint64_t;

// 7.2.3 EFI_BOOT_SERVICES.GetMemoryMap()

using efi_virtual_addr = uint64_t;

struct efi_memory_descriptor {
	uint32_t type;
	efi_physical_addr physical_start;
	efi_virtual_addr virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
};

// 8.3.1 GetTime()

struct efi_time {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t pad1;
	uint32_t nanosecond;
	int16_t time_zone;
	uint8_t daylight;
	uint8_t pad2;
};

// 9.1.1 EFI_LOADED_IMAGE_PROTOCOL

constexpr efi_guid EFI_LOADED_IMAGE_PROTOCOL_GUID = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

struct efi_loaded_image_protocol {
	uint32_t revision;
	efi_handle parent_handle;
	efi_system_table *system_table;
	efi_handle device_handle;
	void *file_path;
	void *reserved;
	uint32_t load_options_size;
	void *load_options;
	void *image_base;
	uint64_t image_size;
	efi_memory_type image_code_type;
	efi_memory_type image_data_type;
	void *unload;
};

// 12.4.1 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL

struct efi_simple_text_output_protocol {
	void *reset;
	efi_status (*output_string)(struct efi_simple_text_output_protocol *self, char16_t *string);
	void *test_string;
	void *query_mode;
	void *set_mode;
	void *set_attribute;
	efi_status (*clear_screen)(struct efi_simple_text_output_protocol *self);
	void *set_cursor_position;
	void *enable_cursor;
	void *mode;
};

// 12.9.2 EFI_GRAPHICS_OUTPUT_PROTOCOL

constexpr efi_guid EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

struct efi_graphics_output_protocol_mode;

struct efi_graphics_output_protocol {
	void *query_mode;
	void *set_mode;
	void *blt;
	struct efi_graphics_output_protocol_mode *mode;
};

struct efi_pixel_bitmask {
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
	uint32_t reserved_mask;
};

enum efi_graphics_pixel_format {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
};

struct efi_graphics_output_mode_information {
	uint32_t version;
	uint32_t horizontal_resolution;
	uint32_t vertical_resolution;
	efi_graphics_pixel_format pixel_format;
	efi_pixel_bitmask pixel_information;
	uint32_t pixels_per_scan_line;
};

struct efi_graphics_output_protocol_mode {
	uint32_t max_mode;
	uint32_t mode;
	efi_graphics_output_mode_information *info;
	size_t size_of_info;
	efi_physical_addr framebuffer_base;
	size_t framebuffer_size;
};

// 13.4.1 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL

constexpr efi_guid EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = { 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

struct efi_file_protocol;

struct efi_simple_file_system_protocol {
	uint64_t revision;
	efi_status (*open_volume)(struct efi_simple_file_system_protocol *self, struct efi_file_protocol **root);
};

// 13.5.1 EFI_FILE_PROTOCOL

struct efi_file_protocol {
	uint64_t revision;
	efi_status (*open)(struct efi_file_protocol *self, struct efi_file_protocol **new_handle, char16_t *file_name, uint64_t open_mode, uint64_t attributes);
	void *close;
	void *del;
	efi_status (*read)(struct efi_file_protocol *self, size_t *buffer_size, void *buffer);
	void *write;
	efi_status (*get_position)(struct efi_file_protocol *self, uint64_t *position);
	efi_status (*set_position)(struct efi_file_protocol *self, uint64_t position);
	efi_status (*get_info)(struct efi_file_protocol *self, efi_guid *information_type, size_t *buffer_size, void *buffer);
	void *set_info;
	void *flush;
	void *open_ex;
	void *read_ex;
	void *write_ex;
	void *flush_ex;
};

// 13.5.2 EFI_FILE_PROTOCOL.Open()

constexpr uint64_t EFI_FILE_MODE_READ = 0x0000000000000001;
constexpr uint64_t EFI_FILE_MODE_WRITE = 0x0000000000000002;
constexpr uint64_t EFI_FILE_MODE_CREATE = 0x8000000000000000;

constexpr uint64_t EFI_FILE_READ_ONLY = 0x0000000000000001;
constexpr uint64_t EFI_FILE_HIDDEN = 0x0000000000000002;
constexpr uint64_t EFI_FILE_SYSTEM = 0x0000000000000004;
constexpr uint64_t EFI_FILE_RESERVED = 0x0000000000000008;
constexpr uint64_t EFI_FILE_DIRECTORY = 0x0000000000000010;
constexpr uint64_t EFI_FILE_ARCHIVE = 0x0000000000000020;
constexpr uint64_t EFI_FILE_VALID_ATTR = 0x0000000000000037;

// 13.5.16 EFI_FILE_INFO

constexpr efi_guid EFI_FILE_INFO_GUID = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

struct efi_file_info {
	uint64_t size;
	uint64_t file_size;
	uint64_t physical_size;
	efi_time create_time;
	efi_time last_access_time;
	efi_time modification_time;
	uint64_t attribute;
	char16_t file_name[];
};

// Appendix D

constexpr efi_status EFI_SUCCESS = 0;

constexpr efi_status EFI_WARN_UNKNOWN_GLYPH = 1;
constexpr efi_status EFI_WARN_DELETE_FAILURE = 2;
constexpr efi_status EFI_WARN_WRITE_FAILURE = 3;
constexpr efi_status EFI_WARN_BUFFER_TOO_SMALL = 4;
constexpr efi_status EFI_WARN_STALE_DATA = 5;
constexpr efi_status EFI_WARN_FILE_SYSTEM = 6;
constexpr efi_status EFI_WARN_RESET_REQUIRED = 7;

constexpr efi_status EFI_LOAD_ERROR = (INTPTR_MAX + 1ULL) + 1;
constexpr efi_status EFI_INVALID_PARAMETER = (INTPTR_MAX + 1ULL) + 2;
constexpr efi_status EFI_UNSUPPORTED = (INTPTR_MAX + 1ULL) + 3;
constexpr efi_status EFI_BAD_BUFFER_SIZE = (INTPTR_MAX + 1ULL) + 4;
constexpr efi_status EFI_BUFFER_TOO_SMALL = (INTPTR_MAX + 1ULL) + 5;
constexpr efi_status EFI_NOT_READY = (INTPTR_MAX + 1ULL) + 6;
constexpr efi_status EFI_DEVICE_ERROR = (INTPTR_MAX + 1ULL) + 7;
constexpr efi_status EFI_WRITE_PROTECTED = (INTPTR_MAX + 1ULL) + 8;
constexpr efi_status EFI_OUT_OF_RESOURCES = (INTPTR_MAX + 1ULL) + 9;
constexpr efi_status EFI_VOLUME_CORRUPTED = (INTPTR_MAX + 1ULL) + 10;
constexpr efi_status EFI_VOLUME_FULL = (INTPTR_MAX + 1ULL) + 11;
constexpr efi_status EFI_NO_MEDIA = (INTPTR_MAX + 1ULL) + 12;
constexpr efi_status EFI_MEDIA_CHANGED = (INTPTR_MAX + 1ULL) + 13;
constexpr efi_status EFI_NOT_FOUND = (INTPTR_MAX + 1ULL) + 14;
constexpr efi_status EFI_ACCESS_DENIED = (INTPTR_MAX + 1ULL) + 15;
constexpr efi_status EFI_NO_RESPONSE = (INTPTR_MAX + 1ULL) + 16;
constexpr efi_status EFI_NO_MAPPING = (INTPTR_MAX + 1ULL) + 17;
constexpr efi_status EFI_TIMEOUT = (INTPTR_MAX + 1ULL) + 18;
constexpr efi_status EFI_NOT_STARTED = (INTPTR_MAX + 1ULL) + 19;
constexpr efi_status EFI_ALREADY_STARTED = (INTPTR_MAX + 1ULL) + 20;
constexpr efi_status EFI_ABORTED = (INTPTR_MAX + 1ULL) + 21;
constexpr efi_status EFI_ICMP_ERROR = (INTPTR_MAX + 1ULL) + 22;
constexpr efi_status EFI_TFTP_ERROR = (INTPTR_MAX + 1ULL) + 23;
constexpr efi_status EFI_PROTOCOL_ERROR = (INTPTR_MAX + 1ULL) + 24;
constexpr efi_status EFI_INCOMPATIBLE_VERSION = (INTPTR_MAX + 1ULL) + 25;
constexpr efi_status EFI_SECURITY_VIOLATION = (INTPTR_MAX + 1ULL) + 26;
constexpr efi_status EFI_CRC_ERROR = (INTPTR_MAX + 1ULL) + 27;
constexpr efi_status EFI_END_OF_MEDIA = (INTPTR_MAX + 1ULL) + 28;
constexpr efi_status EFI_END_OF_FILE = (INTPTR_MAX + 1ULL) + 31;
constexpr efi_status EFI_INVALID_LANGUAGE = (INTPTR_MAX + 1ULL) + 32;
constexpr efi_status EFI_COMPROMISED_DATA = (INTPTR_MAX + 1ULL) + 33;
constexpr efi_status EFI_IP_ADDRESS_CONFLICT = (INTPTR_MAX + 1ULL) + 34;
constexpr efi_status EFI_HTTP_ERROR = (INTPTR_MAX + 1ULL) + 35;

// Related Documents: RISC-V EFI Boot Protocol

constexpr efi_guid RISCV_EFI_BOOT_PROTOCOL_GUID = { 0xccd15fec, 0x6f73, 0x4eec, { 0x83, 0x95, 0x3e, 0x69, 0xe4, 0xb9, 0x40, 0xbf } };

struct riscv_efi_boot_protocol {
	uint64_t revision;
	efi_status (*get_boot_hartid)(riscv_efi_boot_protocol *self, size_t *boot_hart_id);
};

// ------------------------------------------------------------------------------------------------
// Some things heavily rely on declarations later in the spec, so place them last.
// ------------------------------------------------------------------------------------------------

// 4.4.1 EFI_BOOT_SERVICES

struct efi_boot_services {
	efi_table_header hdr;
	void *raise_tpl;
	void *restore_tpl;
	efi_status (*allocate_pages)(efi_allocate_type type, efi_memory_type memory_type, size_t pages, efi_physical_addr *memory);
	void *free_pages;
	efi_status (*get_memory_map)(size_t *memory_map_size, efi_memory_descriptor *memory_map, size_t *map_key, size_t *descriptor_size, uint32_t *descriptor_version);
	efi_status (*allocate_pool)(efi_memory_type pool_type, size_t size, void **buffer);
	void *free_pool;
	void *create_event;
	void *set_timer;
	void *wait_for_event;
	void *signal_event;
	void *close_event;
	void *check_event;
	void *install_protocol_interface;
	void *reinstall_protocol_interface;
	void *uninstall_protocol_interface;
	efi_status (*handle_protocol)(efi_handle handle, efi_guid *protocol, void **interface);
	void *reserved;
	void *register_protocol_notify;
	void *locate_handle;
	void *locate_device_path;
	void *install_configuration_table;
	void *load_image;
	void *start_image;
	void *exit;
	void *unload_image;
	efi_status (*exit_boot_services)(efi_handle image_handle, size_t map_key);
	void *get_next_monotonic_count;
	void *stall;
	efi_status (*set_watchdog_timer)(size_t timeout, uint64_t watchdog_code, size_t data_size, char16_t *watchdog_data);
	void *connect_controller;
	void *disconnect_controller;
	void *open_protocol;
	void *close_protocol;
	void *open_protocol_information;
	void *protocols_per_handle;
	void *locate_handle_buffer;
	efi_status (*locate_protocol)(efi_guid *protocol, void *registration, void **interface);
	void *install_multiple_protocol_interface;
	void *uninstall_multiple_protocol_interface;
	void *calculate_crc32;
	void *copy_mem;
	void *set_mem;
	void *create_event_ex;
};
