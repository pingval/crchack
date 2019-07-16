#ifndef CRCHACK_H
#define CRCHACK_H

#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define DEBUG 0
// #define TARGETS_SIZE (0x2000 * 16) // PlayStation MemoryCard size
#define TARGETS_SIZE (1024 * 1024 * 10) // 10 MB

extern long TARGETS[TARGETS_SIZE];

#endif
