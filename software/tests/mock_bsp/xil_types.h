/* HOST TEST ONLY: same-width public scalar types, not target BSP. */
#ifndef MOCK_XIL_TYPES_H
#define MOCK_XIL_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef int8_t s8;typedef int16_t s16;typedef int32_t s32;typedef int64_t s64;
typedef uintptr_t UINTPTR;typedef intptr_t INTPTR;typedef char char8;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define UPPER_32_BITS(x) ((u32)((u64)(x)>>32U))
#define LOWER_32_BITS(x) ((u32)(x))
#define LEFT_SHIFT_BY_32_BITS(x) ((u64)(x)<<32U)
#define XIL_COMPONENT_IS_READY 0x11111111U
#define XIL_COMPONENT_IS_STARTED 0x22222222U
#endif
