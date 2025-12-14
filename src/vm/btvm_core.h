// btvm_core.h — Common definitions for BTBC
//
// This header contains shared structures, constants, and inline functions
//

#ifndef BTVM_CORE_H
#define BTVM_CORE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(_MSC_VER)
#  pragma pack(push, 1)
#endif

typedef struct
#if !defined(_MSC_VER)
__attribute__((packed))
#endif
{
    char     magic[4];       // "BTBC"
    uint16_t version;        // 2
    uint16_t flags;          // 0
    uint32_t entry_code_off; // offset in Code section
    uint32_t const_off, const_size;
    uint32_t type_off,  type_size;
    uint32_t gdata_size;     // bytes
    uint32_t global_base;
    uint32_t reserved0;      // reserved field
    uint32_t ftab_off,  ftab_size;
    uint32_t code_off,  code_size;
    uint32_t dbg_off,   dbg_size;
} BTBC_Header;  // 64 bytes

typedef struct
#if !defined(_MSC_VER)
__attribute__((packed))
#endif
{
    uint32_t id;
    uint32_t name_idx;
    uint32_t level;
    uint32_t has_sl;
    uint32_t locals_size;
    uint32_t args_bytes;
    uint32_t code_off;
    uint32_t code_size;
} BTBC_Func;  // 32 bytes

#if defined(_MSC_VER)
#  pragma pack(pop)
#endif

enum {
    OPAdd   = 0,
    OPNeg   = 1,
    OPMul   = 2,
    OPDivD  = 3,
    OPRemD  = 4,
    OPDiv2  = 5,
    OPRem2  = 6,
    OPEqlI  = 7,
    OPNEqI  = 8,
    OPLssI  = 9,
    OPLeqI  = 10,
    OPGtrI  = 11,
    OPGEqI  = 12,
    OPDupl  = 13,
    OPSwap  = 14,
    OPAndB  = 15,
    OPOrB   = 16,
    OPLoad  = 17,
    OPStore = 18,
    OPHalt  = 19,
    OPWrI   = 20,
    OPWrC   = 21,
    OPWrL   = 22,
    OPRdI   = 23,
    OPRdC   = 24,
    OPRdL   = 25,
    OPEOF   = 26,
    OPEOL   = 27,
    OPLdC   = 28,
    OPLdA   = 29,
    OPLdLA  = 30,
    OPLdL   = 31,
    OPLdG   = 32,
    OPStL   = 33,
    OPStG   = 34,
    OPMove  = 35,
    OPCopy  = 36,
    OPAddC  = 37,
    OPMulC  = 38,
    OPJmp   = 39,
    OPJZ    = 40,
    OPCall  = 41,
    OPAdjS  = 42,
    OPExit  = 43
};

// Opcode names for disassembly
static const char* opcode_names[] = {
    "OPAdd",   // 0
    "OPNeg",   // 1
    "OPMul",   // 2
    "OPDivD",  // 3
    "OPRemD",  // 4
    "OPDiv2",  // 5
    "OPRem2",  // 6
    "OPEqlI",  // 7
    "OPNEqI",  // 8
    "OPLssI",  // 9
    "OPLeqI",  // 10
    "OPGtrI",  // 11
    "OPGEqI",  // 12
    "OPDupl",  // 13
    "OPSwap",  // 14
    "OPAndB",  // 15
    "OPOrB",   // 16
    "OPLoad",  // 17
    "OPStore", // 18
    "OPHalt",  // 19
    "OPWrI",   // 20
    "OPWrC",   // 21
    "OPWrL",   // 22
    "OPRdI",   // 23
    "OPRdC",   // 24
    "OPRdL",   // 25
    "OPEOF",   // 26
    "OPEOL",   // 27
    "OPLdC",   // 28
    "OPLdA",   // 29
    "OPLdLA",  // 30
    "OPLdL",   // 31
    "OPLdG",   // 32
    "OPStL",   // 33
    "OPStG",   // 34
    "OPMove",  // 35
    "OPCopy",  // 36
    "OPAddC",  // 37
    "OPMulC",  // 38
    "OPJmp",   // 39
    "OPJZ",    // 40
    "OPCall",  // 41
    "OPAdjS",  // 42
    "OPExit"   // 43
};

// check if opcode has 32-bit immediate operand
static inline bool has_imm32(uint8_t op) { 
    return op >= 28; 
}

// error handling
static inline void btvm_die(const char* msg) {
    fprintf(stderr, "btvm: %s\n", msg);
    exit(2);
}

static inline void btvm_dief(const char* fmt, uint32_t a, uint32_t b) {
    fprintf(stderr, "btvm: ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
    exit(2);
}

// safe memory access
static inline int32_t mem_read32(const uint8_t* mem, uint32_t mem_size, uint32_t addr) {
    if (addr + 4u > mem_size) 
        btvm_dief("mem_read32 OOB addr=%u size=%u", addr, mem_size);
    int32_t v;
    memcpy(&v, mem + addr, 4);
    return v;
}

static inline void mem_write32(uint8_t* mem, uint32_t mem_size, uint32_t addr, int32_t v) {
    if (addr + 4u > mem_size) 
        btvm_dief("mem_write32 OOB addr=%u size=%u", addr, mem_size);
    memcpy(mem + addr, &v, 4);
}

static inline int32_t fetch_i32(const uint8_t* code, uint32_t code_size, uint32_t* pc) {
    if (*pc + 4u > code_size) 
        btvm_die("fetch_i32 truncated");
    int32_t v;
    memcpy(&v, code + *pc, 4);
    *pc += 4u;
    return v;
}

// I/O helpers
static inline void vm_print_int_width(int32_t value, int32_t width) {
    if (width < 1) width = 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", value);
    int len = (int)strlen(buf);
    for (int i = len; i < width; i++) putchar(' ');
    fputs(buf, stdout);
}

static inline int vm_peek_stdin(void) {
    int c = fgetc(stdin);
    if (c != EOF) ungetc(c, stdin);
    return c;
}

#endif // BTVM_CORE_H
