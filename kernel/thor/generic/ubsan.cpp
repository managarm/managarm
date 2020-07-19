#include <stdint.h>

#include <thor-internal/debug.hpp>

namespace {
	struct type_descriptor {
		uint16_t type_kind;
		uint16_t type_info;
		char type_name[];
	};

	struct source_location {
		char *filename;
		uint32_t line;
		uint32_t column;
	};

	struct overflow_data {
		struct source_location loc;
		struct type_descriptor *type;
	};

	struct shift_out_of_bounds_data {
		struct source_location loc;
		struct type_descriptor *lhs_type;
		struct type_descriptor *rhs_type;
	};

	struct invalid_value_data {
		struct source_location loc;
		struct type_descriptor *type;
	};

	struct out_of_bounds_data {
		struct source_location loc;
		struct type_descriptor *array_type;
		struct type_descriptor *index_type;
	};

	struct type_mismatch_data_v1 {
		struct source_location loc;
		struct type_descriptor *type;
		unsigned char log_alignment;
		unsigned char type_check_kind;
	};

	struct vla_bound_data {
		struct source_location loc;
		struct type_descriptor *type;
	};

	struct nonnull_return_data {
		struct source_location attr_loc;
	};

	struct nonnull_arg_data {
		struct source_location loc;
	};

	struct unreachable_data {
		struct source_location loc;
	};

	struct invalid_builtin_data {
		struct source_location loc;
		unsigned char kind;
	};

	void log_location(struct source_location loc) {
		thor::infoLogger() << "thor: UBSAN failure at "
				<< loc.filename << ":" << loc.line << frg::endlog;
	}
}

extern "C" void __ubsan_handle_add_overflow(struct overflow_data *data,
		uintptr_t lhs, uintptr_t rhs) {
	thor::infoLogger() << "thor: UBSAN failure, addition overflow" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_sub_overflow(struct overflow_data *data,
		uintptr_t lhs, uintptr_t rhs) {
	thor::infoLogger() << "thor: UBSAN failure, subtraction overflow" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_mul_overflow(struct overflow_data *data,
		uintptr_t lhs, uintptr_t rhs) {
	thor::infoLogger() << "thor: UBSAN failure, multiplication overflow" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_divrem_overflow(struct overflow_data *data,
		uintptr_t lhs, uintptr_t rhs) {
	thor::infoLogger() << "thor: UBSAN failure, division overflow" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_negate_overflow(struct overflow_data *data,
		uintptr_t operand) {
	thor::infoLogger() << "thor: UBSAN failure, negation overflow" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_pointer_overflow(struct overflow_data *data,
		uintptr_t base, uintptr_t result) {
	thor::infoLogger() << "thor: UBSAN failure, pointer overflow"
			<< " from " << (void *)base << " to " << (void *)result << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_shift_out_of_bounds(struct shift_out_of_bounds_data *data,
		uintptr_t lhs, uintptr_t rhs) {
	thor::infoLogger() << "thor: UBSAN failure, shift overflow" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_load_invalid_value(struct invalid_value_data *data,
		uintptr_t value) {
	thor::infoLogger() << "thor: UBSAN failure, load of invalid value" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_out_of_bounds(struct out_of_bounds_data *data,
		uintptr_t index) {
	thor::infoLogger() << "thor: UBSAN failure, array index out of bounds" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_type_mismatch_v1(struct type_mismatch_data_v1 *data,
		uintptr_t ptr) {
	if(!ptr) {
		thor::infoLogger() << "thor: UBSAN failure, null pointer access" << frg::endlog;
	} else if (ptr & ((1 << data->log_alignment) - 1)) {
		thor::infoLogger() << "thor: UBSAN failure, use of misaligned pointer" << frg::endlog;
	} else {
		thor::infoLogger() << "thor: UBSAN failure, insufficient space for object" << frg::endlog;
	}
	log_location(data->loc);
}

extern "C" void __ubsan_handle_vla_bound_not_positive(struct vla_bound_data *data,
		uintptr_t bound) {
	thor::infoLogger() << "thor: UBSAN failure, negative VLA size" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_nonnull_return(struct non_null_return_data *data,
		struct source_location *loc) {
	thor::infoLogger() << "thor: UBSAN failure, non-null return is null" << frg::endlog;
	log_location(*loc);
}

extern "C" void __ubsan_handle_nonnull_arg(struct nonnull_arg_data *data) {
	thor::infoLogger() << "thor: UBSAN failure, non-null argument is null" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_builtin_unreachable(struct unreachable_data *data) {
	thor::infoLogger() << "thor: UBSAN failure, unreachable code is reached" << frg::endlog;
	log_location(data->loc);
}

extern "C" void __ubsan_handle_invalid_builtin(struct invalid_builtin_data *data) {
	thor::infoLogger() << "thor: UBSAN failure, invalid invocation of builtin" << frg::endlog;
	log_location(data->loc);
}
