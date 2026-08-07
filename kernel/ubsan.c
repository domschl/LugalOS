#include "kernel/printk.h"
#include <stdint.h>

struct ubsan_source_location {
    const char *filename;
    uint32_t line;
    uint32_t column;
};

struct ubsan_type_descriptor {
    uint16_t type_kind;
    uint16_t type_info;
    char type_name[1];
};

struct ubsan_out_of_bounds_data {
    struct ubsan_source_location location;
    struct ubsan_type_descriptor *array_type;
    struct ubsan_type_descriptor *index_type;
};

struct ubsan_shift_out_of_bounds_data {
    struct ubsan_source_location location;
    struct ubsan_type_descriptor *lhs_type;
    struct ubsan_type_descriptor *rhs_type;
};

struct ubsan_type_mismatch_data_v1 {
    struct ubsan_source_location location;
    struct ubsan_type_descriptor *type;
    uint8_t log_alignment;
    uint8_t type_check_kind;
};

struct ubsan_overflow_data {
    struct ubsan_source_location location;
    struct ubsan_type_descriptor *type;
};

struct ubsan_unreachable_data {
    struct ubsan_source_location location;
};

static void print_location(const struct ubsan_source_location *loc) {
    if (loc && loc->filename) {
        printk("[%s:%u:%u] ", loc->filename, (unsigned int)loc->line, (unsigned int)loc->column);
    } else {
        printk("[unknown location] ");
    }
}

/* With -fno-sanitize-recover=all the compiler assumes these handlers never
 * return; historically they did anyway (log-and-continue), which let
 * undefined behavior keep executing after being detected. When
 * CONFIG_UBSAN_PANIC is set, halt instead so a fault is a deterministic stop. */
#if defined(CONFIG_UBSAN_PANIC)
static void ubsan_halt(void) {
    printk("[UBSan Fatal] Halting system due to undefined behavior.\n");
    while (1) {
        __asm__ __volatile__("wfi");
    }
}
#define UBSAN_MAYBE_HALT() ubsan_halt()
#else
#define UBSAN_MAYBE_HALT() ((void)0)
#endif

void __ubsan_handle_out_of_bounds(struct ubsan_out_of_bounds_data *data, uintptr_t index) {
    printk("\n[UBSan Fault] Out-of-bounds array access! Index: %u ", (unsigned int)index);
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_shift_out_of_bounds(struct ubsan_shift_out_of_bounds_data *data, uintptr_t lhs, uintptr_t rhs) {
    (void)lhs;
    printk("\n[UBSan Fault] Shift out of bounds! Shift count: %u ", (unsigned int)rhs);
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_type_mismatch_v1(struct ubsan_type_mismatch_data_v1 *data, uintptr_t ptr) {
    if (ptr == 0) {
        printk("\n[UBSan Fault] NULL pointer dereference! ");
    } else if (data->log_alignment && (ptr & ((1 << data->log_alignment) - 1))) {
        printk("\n[UBSan Fault] Misaligned memory access at 0x%x! ", (unsigned int)ptr);
    } else {
        printk("\n[UBSan Fault] Type mismatch at 0x%x! ", (unsigned int)ptr);
    }
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_add_overflow(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    (void)lhs; (void)rhs;
    printk("\n[UBSan Fault] Signed integer addition overflow! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_sub_overflow(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    (void)lhs; (void)rhs;
    printk("\n[UBSan Fault] Signed integer subtraction overflow! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_mul_overflow(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    (void)lhs; (void)rhs;
    printk("\n[UBSan Fault] Signed integer multiplication overflow! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_negate_overflow(struct ubsan_overflow_data *data, uintptr_t old_val) {
    (void)old_val;
    printk("\n[UBSan Fault] Integer negation overflow! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_divrem_overflow(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    (void)lhs; (void)rhs;
    printk("\n[UBSan Fault] Integer division/remainder overflow! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_builtin_unreachable(struct ubsan_unreachable_data *data) {
    printk("\n[UBSan Fault] Reached unreachable code! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_pointer_overflow(struct ubsan_overflow_data *data, uintptr_t base, uintptr_t result) {
    (void)base; (void)result;
    printk("\n[UBSan Fault] Pointer arithmetic overflow! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_out_of_bounds_abort(struct ubsan_out_of_bounds_data *data, uintptr_t index) {
    __ubsan_handle_out_of_bounds(data, index);
}

void __ubsan_handle_shift_out_of_bounds_abort(struct ubsan_shift_out_of_bounds_data *data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_shift_out_of_bounds(data, lhs, rhs);
}

void __ubsan_handle_type_mismatch_v1_abort(struct ubsan_type_mismatch_data_v1 *data, uintptr_t ptr) {
    __ubsan_handle_type_mismatch_v1(data, ptr);
}

void __ubsan_handle_add_overflow_abort(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_add_overflow(data, lhs, rhs);
}

void __ubsan_handle_sub_overflow_abort(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_sub_overflow(data, lhs, rhs);
}

void __ubsan_handle_mul_overflow_abort(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_mul_overflow(data, lhs, rhs);
}

void __ubsan_handle_negate_overflow_abort(struct ubsan_overflow_data *data, uintptr_t old_val) {
    __ubsan_handle_negate_overflow(data, old_val);
}

void __ubsan_handle_divrem_overflow_abort(struct ubsan_overflow_data *data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_divrem_overflow(data, lhs, rhs);
}

void __ubsan_handle_builtin_unreachable_abort(struct ubsan_unreachable_data *data) {
    __ubsan_handle_builtin_unreachable(data);
}

void __ubsan_handle_pointer_overflow_abort(struct ubsan_overflow_data *data, uintptr_t base, uintptr_t result) {
    __ubsan_handle_pointer_overflow(data, base, result);
}

struct ubsan_invalid_value_data {
    struct ubsan_source_location location;
    struct ubsan_type_descriptor *type;
};

void __ubsan_handle_load_invalid_value(struct ubsan_invalid_value_data *data, uintptr_t val) {
    (void)val;
    printk("\n[UBSan Fault] Load of invalid value! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_load_invalid_value_abort(struct ubsan_invalid_value_data *data, uintptr_t val) {
    __ubsan_handle_load_invalid_value(data, val);
}

void __ubsan_handle_nonnull_arg(struct ubsan_invalid_value_data *data) {
    printk("\n[UBSan Fault] Nonnull argument is null! ");
    print_location(&data->location);
    printk("\n");
    UBSAN_MAYBE_HALT();
}

void __ubsan_handle_nonnull_arg_abort(struct ubsan_invalid_value_data *data) {
    __ubsan_handle_nonnull_arg(data);
}


