#ifndef CRCHACK_H
#define CRCHACK_H

#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define DEBUG 0
// #define FLIP_TABLE_SIZE (0x2000 * 16) // PlayStation MemoryCard size
#define FLIP_TABLE_SIZE (1024 * 1024 * 10) // 10 MB

extern long FLIP_TABLE[FLIP_TABLE_SIZE];

#endif
