// btvm.c — minimal VM for BTBC
//
// Build:
//   cc -O2 -std=c11 -Wall -Wextra -pedantic -o btvm btvm.c
//
// Run:
//   ./btvm program.btbc
//   ./btvm --trace program.btbc
//

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
    uint32_t reserved0;      // Reserved0 field
    uint32_t ftab_off,  ftab_size;
    uint32_t code_off,  code_size;
    uint32_t dbg_off,   dbg_size;
} BTBC_Header;  // Total: 64 bytes

typedef struct
#if !defined(_MSC_VER)
__attribute__((packed))
#endif
{
    uint32_t id;
    uint32_t name_idx;     // usually 0xFFFFFFFF
    uint32_t level;
    uint32_t has_sl;
    uint32_t locals_size;
    uint32_t args_bytes;
    uint32_t code_off;
    uint32_t code_size;
} BTBC_Func;  // 32 bytes total

#if defined(_MSC_VER)
#  pragma pack(pop)
#endif

// Opcodes
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

static inline bool has_imm32(uint8_t op) { return op >= 28; }

static void die(const char* msg) {
    fprintf(stderr, "btvm: %s\n", msg);
    exit(2);
}

static void dief(const char* fmt, uint32_t a, uint32_t b) {
    fprintf(stderr, "btvm: ");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
    exit(2);
}

// безопасные read/write 32-bit без требований к выравниванию
static inline int32_t mem_read32(const uint8_t* mem, uint32_t mem_size, uint32_t addr) {
    if (addr + 4u > mem_size) dief("mem_read32 OOB addr=%u size=%u", addr, mem_size);
    int32_t v;
    memcpy(&v, mem + addr, 4);
    return v;
}

static inline void mem_write32(uint8_t* mem, uint32_t mem_size, uint32_t addr, int32_t v) {
    if (addr + 4u > mem_size) dief("mem_write32 OOB addr=%u size=%u", addr, mem_size);
    memcpy(mem + addr, &v, 4);
}

static inline int32_t fetch_i32(const uint8_t* code, uint32_t code_size, uint32_t* pc) {
    if (*pc + 4u > code_size) die("fetch_i32 truncated");
    int32_t v;
    memcpy(&v, code + *pc, 4);
    *pc += 4u;
    return v;
}

// VM State
typedef struct {
    BTBC_Header hdr;
    BTBC_Func*  funcs;
    uint32_t    func_count;

    uint8_t* code;
    uint32_t code_size;

    // unified address space: [0..gdata_size) = globals, [stack_base..stack_base+stack_size) = stack bytes
    uint8_t* mem;
    uint32_t mem_size;

    uint32_t stack_base;
    uint32_t stack_size;

    uint32_t sp;      // like ESP: points to top dword (lowest addr). Stack grows downward
    uint32_t ebp;     // base for globals (0 in our layout)

    bool trace;
} VM;

static void vm_load_file(VM* vm, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) die("cannot open file");

    // read header
    if (fread(&vm->hdr, 1, sizeof(BTBC_Header), f) != sizeof(BTBC_Header)) die("cannot read header");

    if (memcmp(vm->hdr.magic, "BTBC", 4) != 0) die("bad magic");
    if (vm->hdr.version != 2) die("unsupported version");

    // func table
    if (vm->hdr.ftab_size % sizeof(BTBC_Func) != 0) die("bad function table size");
    vm->func_count = vm->hdr.ftab_size / (uint32_t)sizeof(BTBC_Func);
    if (vm->func_count > 0) {
        vm->funcs = (BTBC_Func*)malloc(vm->hdr.ftab_size);
        if (!vm->funcs) die("OOM funcs");
        if (fseek(f, (long)vm->hdr.ftab_off, SEEK_SET) != 0) die("seek ftab failed");
        if (fread(vm->funcs, 1, vm->hdr.ftab_size, f) != vm->hdr.ftab_size) die("read ftab failed");
    } else {
        vm->funcs = NULL;
    }

    // code section
    vm->code_size = vm->hdr.code_size;
    vm->code = (uint8_t*)malloc(vm->code_size);
    if (!vm->code) die("OOM code");
    if (fseek(f, (long)vm->hdr.code_off, SEEK_SET) != 0) die("seek code failed");
    if (fread(vm->code, 1, vm->code_size, f) != vm->code_size) die("read code failed");

    fclose(f);

    // memory: globals + stack
    uint32_t gsize = (vm->hdr.gdata_size + 3u) & ~3u;

    vm->stack_base = gsize;
    vm->mem_size   = vm->stack_base + vm->stack_size + 4u;

    vm->mem = (uint8_t*)malloc(vm->mem_size);
    if (!vm->mem) die("OOM mem");
    memset(vm->mem, 0, vm->mem_size);

    // EBP = global_base, so that [EBP + negative_offset] maps to valid memory
    // Pascal compiler places globals at negative offsets from EBP
    vm->ebp = vm->hdr.global_base;

    // IMPORTANT: reserve "exit code" cell at top of stack like btpc64 expects (StackPosition := 4).
    vm->sp = vm->stack_base + vm->stack_size - 4u;
    mem_write32(vm->mem, vm->mem_size, vm->sp, 0);
}

// push/pop 32-bit on unified stack
static inline void push32(VM* vm, int32_t v) {
    if (vm->sp < vm->stack_base + 4u) die("stack overflow");
    vm->sp -= 4u;
    mem_write32(vm->mem, vm->mem_size, vm->sp, v);
}

static inline int32_t pop32(VM* vm) {
    if (vm->sp + 4u > vm->stack_base + vm->stack_size) die("stack underflow");
    int32_t v = mem_read32(vm->mem, vm->mem_size, vm->sp);
    vm->sp += 4u;
    return v;
}

static void vm_print_int_width(int32_t value, int32_t width) {
    if (width < 1) width = 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", value);
    int len = (int)strlen(buf);
    for (int i = len; i < width; i++) putchar(' ');
    fputs(buf, stdout);
}

static int vm_peek_stdin(void) {
    int c = fgetc(stdin);
    if (c != EOF) ungetc(c, stdin);
    return c;
}

static int vm_run(VM* vm) {
    uint32_t pc = vm->hdr.entry_code_off;
    const uint8_t* code = vm->code;
    const uint32_t code_size = vm->code_size;

    for (;;) {
        if (pc >= code_size) die("pc out of code");
        uint32_t pc0 = pc;
        uint8_t op = code[pc++];

        int32_t imm = 0;
        if (has_imm32(op)) {
            imm = fetch_i32(code, code_size, &pc);
        }

        if (vm->trace) {
            if (has_imm32(op))
                fprintf(stderr, "pc=%u op=%u imm=%d sp=%u\n", pc0, op, imm, vm->sp);
            else
                fprintf(stderr, "pc=%u op=%u sp=%u\n", pc0, op, vm->sp);
        }

        switch (op) {
            case OPAdd:  { int32_t b=pop32(vm), a=pop32(vm); push32(vm, a+b); } break;
            case OPNeg:  { int32_t a=pop32(vm); push32(vm, -a); } break;
            case OPMul:  { int32_t b=pop32(vm), a=pop32(vm); push32(vm, a*b); } break;
            case OPDivD: { int32_t b=pop32(vm), a=pop32(vm); if (b==0) die("div by 0"); push32(vm, a/b); } break;
            case OPRemD: { int32_t b=pop32(vm), a=pop32(vm); if (b==0) die("mod by 0"); push32(vm, a%b); } break;
            case OPDiv2: { int32_t a=pop32(vm); push32(vm, a/2); } break;
            case OPRem2: { int32_t a=pop32(vm); push32(vm, a%2); } break;

            case OPEqlI: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, (a==b)?1:0); } break;
            case OPNEqI: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, (a!=b)?1:0); } break;
            case OPLssI: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, (a< b)?1:0); } break;
            case OPLeqI: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, (a<=b)?1:0); } break;
            case OPGtrI: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, (a> b)?1:0); } break;
            case OPGEqI: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, (a>=b)?1:0); } break;

            case OPDupl: {
                int32_t a = mem_read32(vm->mem, vm->mem_size, vm->sp);
                push32(vm, a);
            } break;

            case OPSwap: {
                // swap top two dwords at [sp] and [sp+4]
                if (vm->sp + 8u > vm->stack_base + vm->stack_size) die("swap underflow");
                int32_t a = mem_read32(vm->mem, vm->mem_size, vm->sp);
                int32_t b = mem_read32(vm->mem, vm->mem_size, vm->sp + 4u);
                mem_write32(vm->mem, vm->mem_size, vm->sp, b);
                mem_write32(vm->mem, vm->mem_size, vm->sp + 4u, a);
            } break;

            case OPAndB: { int32_t b=pop32(vm), a=pop32(vm); push32(vm, a & b); } break;
            case OPOrB:  { int32_t b=pop32(vm), a=pop32(vm); push32(vm, a | b); } break;

            case OPLoad: {
                uint32_t addr = (uint32_t)pop32(vm);
                int32_t v = mem_read32(vm->mem, vm->mem_size, addr);
                push32(vm, v);
            } break;

            case OPStore: {
                uint32_t addr = (uint32_t)pop32(vm);  // top = addr
                int32_t v = pop32(vm);                // below = value
                mem_write32(vm->mem, vm->mem_size, addr, v);
            } break;

            case OPHalt: {
                // Exit code expected at [ESP] (top dword). We don't pop it.
                int32_t code0 = mem_read32(vm->mem, vm->mem_size, vm->sp);
                return code0;
            }

            case OPWrI: {
                // stack: [... value width] (width on top)
                int32_t width = pop32(vm);
                int32_t value = pop32(vm);
                vm_print_int_width(value, width);
            } break;

            case OPWrC: {
                int32_t v = pop32(vm);
                putchar((unsigned char)(v & 0xFF));
            } break;

            case OPWrL: {
                putchar('\n');
            } break;

            case OPRdI: {
                // top = address to store integer into
                uint32_t addr = (uint32_t)pop32(vm);
                int32_t x = 0;
                if (scanf("%d", &x) != 1) x = 0;
                mem_write32(vm->mem, vm->mem_size, addr, x);
            } break;

            case OPRdC: {
                // top = address to store char code into
                uint32_t addr = (uint32_t)pop32(vm);
                int c = getchar();
                if (c == EOF) c = 0;
                mem_write32(vm->mem, vm->mem_size, addr, (int32_t)(unsigned char)c);
            } break;

            case OPRdL: {
                int c;
                do { c = getchar(); } while (c != '\n' && c != EOF);
            } break;

            case OPEOF: {
                int c = vm_peek_stdin();
                push32(vm, (c == EOF) ? 1 : 0);
            } break;

            case OPEOL: {
                int c = vm_peek_stdin();
                push32(vm, (c == '\n' || c == EOF) ? 1 : 0);
            } break;

            case OPLdC: {
                push32(vm, imm);
            } break;

            case OPLdA: {
                // address = EBP + imm (EBP=0 => imm)
                int64_t addr = (int64_t)vm->ebp + (int64_t)imm;
                if (addr < 0 || (uint64_t)addr >= vm->mem_size) die("OPLdA addr OOB");
                push32(vm, (int32_t)addr);
            } break;

            case OPLdLA: {
                // address = ESP + imm
                int64_t addr = (int64_t)vm->sp + (int64_t)imm;
                if (addr < 0 || (uint64_t)addr >= vm->mem_size) die("OPLdLA addr OOB");
                push32(vm, (int32_t)addr);
            } break;

            case OPLdL: {
                // load [ESP + imm], then push it
                int64_t addr = (int64_t)vm->sp + (int64_t)imm;
                if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) die("OPLdL addr OOB");
                int32_t v = mem_read32(vm->mem, vm->mem_size, (uint32_t)addr);
                push32(vm, v);
            } break;

            case OPLdG: {
                // load [EBP + imm], then push it
                int64_t addr = (int64_t)vm->ebp + (int64_t)imm;
                if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) die("OPLdG addr OOB");
                int32_t v = mem_read32(vm->mem, vm->mem_size, (uint32_t)addr);
                push32(vm, v);
            } break;

            case OPStL: {
                // btpc64 x86: pop eax; Value := Value-4; store [ESP+Value] = eax
                int32_t v = pop32(vm);
                int64_t addr = (int64_t)vm->sp + (int64_t)imm - 4;
                if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) die("OPStL addr OOB");
                mem_write32(vm->mem, vm->mem_size, (uint32_t)addr, v);
            } break;

            case OPStG: {
                int32_t v = pop32(vm);
                int64_t addr = (int64_t)vm->ebp + (int64_t)imm;
                if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) die("OPStG addr OOB");
                mem_write32(vm->mem, vm->mem_size, (uint32_t)addr, v);
            } break;

            case OPMove: {
                // stack: [... src dst] (dst on top): pop edi; pop esi; rep movsb
                uint32_t dst = (uint32_t)pop32(vm);
                uint32_t src = (uint32_t)pop32(vm);
                uint32_t n = (uint32_t)imm;
                if (src + n > vm->mem_size || dst + n > vm->mem_size) die("OPMove OOB");
                memmove(vm->mem + dst, vm->mem + src, n);
            } break;

            case OPCopy: {
                // x86: pop esi (src); sub esp, size; edi=esp; rep movsb
                uint32_t src = (uint32_t)pop32(vm);
                uint32_t n = (uint32_t)imm;
                if (vm->sp < vm->stack_base + n) die("OPCopy stack overflow");
                vm->sp -= n; // allocate bytes on stack
                uint32_t dst = vm->sp;
                if (src + n > vm->mem_size || dst + n > vm->mem_size) die("OPCopy OOB");
                memmove(vm->mem + dst, vm->mem + src, n);
                // note: no address is pushed; bytes are now on stack
            } break;

            case OPAddC: {
                int32_t a = pop32(vm);
                push32(vm, a + imm);
            } break;

            case OPMulC: {
                int32_t a = pop32(vm);
                push32(vm, a * imm);
            } break;

            case OPJmp: {
                // imm is rel32 from next instruction
                int64_t npc = (int64_t)pc + (int64_t)imm;
                if (npc < 0 || (uint64_t)npc > code_size) die("OPJmp target OOB");
                pc = (uint32_t)npc;
            } break;

            case OPJZ: {
                int32_t cond = pop32(vm);
                if (cond == 0) {
                    int64_t npc = (int64_t)pc + (int64_t)imm;
                    if (npc < 0 || (uint64_t)npc > code_size) die("OPJZ target OOB");
                    pc = (uint32_t)npc;
                }
            } break;

            case OPCall: {
                // imm is func_id
                uint32_t id = (uint32_t)imm;
                if (id >= vm->func_count) die("OPCall bad func id");
                // push return address (pc is already after imm)
                push32(vm, (int32_t)pc);
                // jump to function body
                pc = vm->funcs[id].code_off;
                if (pc >= code_size) die("OPCall callee pc OOB");
            } break;

            case OPAdjS: {
                // adjust ESP by signed imm
                int64_t nsp = (int64_t)vm->sp + (int64_t)imm;
                // stack region bounds: [stack_base .. stack_base+stack_size)
                uint32_t lo = vm->stack_base;
                uint32_t hi = vm->stack_base + vm->stack_size;
                if (nsp < (int64_t)lo || nsp > (int64_t)hi) die("OPAdjS out of stack");
                vm->sp = (uint32_t)nsp;
            } break;

            case OPExit: {
                // Return: ret_pc at [ESP], then ESP += imm (imm includes 4 bytes return address)
                uint32_t ret_pc = (uint32_t)mem_read32(vm->mem, vm->mem_size, vm->sp);
                int64_t nsp = (int64_t)vm->sp + (int64_t)imm;
                uint32_t lo = vm->stack_base;
                uint32_t hi = vm->stack_base + vm->stack_size;
                if (nsp < (int64_t)lo || nsp > (int64_t)hi) die("OPExit bad sp");
                vm->sp = (uint32_t)nsp;
                if (ret_pc > code_size) die("OPExit bad return pc");
                pc = ret_pc;
            } break;

            default:
                die("unknown opcode");
        }
    }
}

static void usage(void) {
    fprintf(stderr, "Usage: btvm [--trace] [--stack <bytes>] file.btbc\n");
    exit(1);
}

int main(int argc, char** argv) {
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.stack_size = 8u * 1024u * 1024u; // default 8 MB
    vm.trace = false;

    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            vm.trace = true;
        } else if (strcmp(argv[i], "--stack") == 0) {
            if (i + 1 >= argc) usage();
            vm.stack_size = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (vm.stack_size < 1024u) vm.stack_size = 1024u;
        } else if (argv[i][0] == '-') {
            usage();
        } else {
            path = argv[i];
        }
    }
    if (!path) usage();

    vm_load_file(&vm, path);
    int exit_code = vm_run(&vm);

    free(vm.funcs);
    free(vm.code);
    free(vm.mem);

    return exit_code & 0xFF;
}
