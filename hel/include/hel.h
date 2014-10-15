
enum {
	HEL_CALL_CREATE_MEMORY = 1
};

typedef uint64_t hel_handle_t;
typedef hel_handle_t hel_resource_t;
typedef hel_handle_t hel_descriptor_t;

extern "C" hel_resource_t hel_create_memory(size_t length);

