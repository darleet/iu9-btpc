// btdb.c — Simple console debugger for BTBC
//
// Build:
//   cc -O2 -std=c11 -Wall -Wextra -pedantic -o btdb btdb.c
//
// Run:
//   ./btdb program.btbc
//

#include "btvm_core.h"
#include <ctype.h>

#define MAX_BREAKPOINTS 64

typedef struct {
    uint32_t addr;
    bool enabled;
} Breakpoint;

typedef struct {
    BTBC_Header hdr;
    BTBC_Func*  funcs;
    uint32_t    func_count;

    uint8_t* code;
    uint32_t code_size;

    uint8_t* mem;
    uint32_t mem_size;

    uint32_t stack_base;
    uint32_t stack_size;

    uint32_t sp;
    uint32_t ebp;
    uint32_t pc;
    
    // Debugger state
    Breakpoint breakpoints[MAX_BREAKPOINTS];
    int bp_count;
    bool step_mode;
    bool running;
} VM;

static void vm_load_file(VM* vm, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) btvm_die("cannot open file");

    // read header
    if (fread(&vm->hdr, 1, sizeof(BTBC_Header), f) != sizeof(BTBC_Header)) 
        btvm_die("cannot read header");

    if (memcmp(vm->hdr.magic, "BTBC", 4) != 0) btvm_die("bad magic");
    if (vm->hdr.version != 2) btvm_die("unsupported version");

    // func table
    if (vm->hdr.ftab_size % sizeof(BTBC_Func) != 0) btvm_die("bad function table size");
    vm->func_count = vm->hdr.ftab_size / (uint32_t)sizeof(BTBC_Func);
    if (vm->func_count > 0) {
        vm->funcs = (BTBC_Func*)malloc(vm->hdr.ftab_size);
        if (!vm->funcs) btvm_die("OOM funcs");
        if (fseek(f, (long)vm->hdr.ftab_off, SEEK_SET) != 0) btvm_die("seek ftab failed");
        if (fread(vm->funcs, 1, vm->hdr.ftab_size, f) != vm->hdr.ftab_size) btvm_die("read ftab failed");
    } else {
        vm->funcs = NULL;
    }

    // code section
    vm->code_size = vm->hdr.code_size;
    vm->code = (uint8_t*)malloc(vm->code_size);
    if (!vm->code) btvm_die("OOM code");
    if (fseek(f, (long)vm->hdr.code_off, SEEK_SET) != 0) btvm_die("seek code failed");
    if (fread(vm->code, 1, vm->code_size, f) != vm->code_size) btvm_die("read code failed");

    fclose(f);

    // memory: globals + stack
    uint32_t gsize = (vm->hdr.gdata_size + 3u) & ~3u;

    vm->stack_base = gsize;
    vm->mem_size   = vm->stack_base + vm->stack_size + 4u;

    vm->mem = (uint8_t*)malloc(vm->mem_size);
    if (!vm->mem) btvm_die("OOM mem");
    memset(vm->mem, 0, vm->mem_size);

    vm->ebp = vm->hdr.global_base;
    vm->sp = vm->stack_base + vm->stack_size - 4u;
    mem_write32(vm->mem, vm->mem_size, vm->sp, 0);
    
    vm->pc = vm->hdr.entry_code_off;
}

// stack

static inline void push32(VM* vm, int32_t v) {
    if (vm->sp < vm->stack_base + 4u) btvm_die("stack overflow");
    vm->sp -= 4u;
    mem_write32(vm->mem, vm->mem_size, vm->sp, v);
}

static inline int32_t pop32(VM* vm) {
    if (vm->sp + 4u > vm->stack_base + vm->stack_size) btvm_die("stack underflow");
    int32_t v = mem_read32(vm->mem, vm->mem_size, vm->sp);
    vm->sp += 4u;
    return v;
}

// disassembly and display

static uint32_t disassemble_one(VM* vm, uint32_t pc) {
    if (pc >= vm->code_size) {
        printf("  [PC out of bounds]\n");
        return pc;
    }
    
    uint8_t op = vm->code[pc];
    uint32_t next_pc = pc + 1;
    
    printf("  %04x: ", pc);
    
    if (op < sizeof(opcode_names)/sizeof(opcode_names[0])) {
        printf("%-8s", opcode_names[op]);
    } else {
        printf("OP_%02x   ", op);
    }
    
    if (has_imm32(op)) {
        if (next_pc + 4 > vm->code_size) {
            printf(" [truncated]\n");
            return next_pc;
        }
        int32_t imm = fetch_i32(vm->code, vm->code_size, &next_pc);
        printf(" %d (0x%x)", imm, (uint32_t)imm);
    }
    
    printf("\n");
    return next_pc;
}

static void show_current_instruction(VM* vm) {
    printf("\n=> ");
    disassemble_one(vm, vm->pc);
}

static void show_stack(VM* vm, int count) {
    printf("\nStack (SP=%u, count=%d):\n", vm->sp, count);
    
    if (count <= 0) count = 8;
    
    uint32_t addr = vm->sp;
    for (int i = 0; i < count && addr < vm->stack_base + vm->stack_size; i++) {
        if (addr + 4 > vm->mem_size) break;
        int32_t val = mem_read32(vm->mem, vm->mem_size, addr);
        printf("  [%04x] = %d (0x%08x)\n", addr, val, (uint32_t)val);
        addr += 4;
    }
}

static void show_registers(VM* vm) {
    printf("\nRegisters:\n");
    printf("  PC  = %u (0x%x)\n", vm->pc, vm->pc);
    printf("  SP  = %u (0x%x)\n", vm->sp, vm->sp);
    printf("  EBP = %u (0x%x)\n", vm->ebp, vm->ebp);
}

static void show_memory(VM* vm, uint32_t addr, int count) {
    printf("\nMemory at 0x%x:\n", addr);
    
    if (count <= 0) count = 16;
    
    for (int i = 0; i < count; i += 4) {
        if (addr + i + 4 > vm->mem_size) {
            printf("  [out of bounds]\n");
            break;
        }
        int32_t val = mem_read32(vm->mem, vm->mem_size, addr + i);
        printf("  [%04x] = %d (0x%08x)\n", addr + i, val, (uint32_t)val);
    }
}

// breakpoints

static void add_breakpoint(VM* vm, uint32_t addr) {
    if (vm->bp_count >= MAX_BREAKPOINTS) {
        printf("Too many breakpoints (max %d)\n", MAX_BREAKPOINTS);
        return;
    }
    
    for (int i = 0; i < vm->bp_count; i++) {
        if (vm->breakpoints[i].addr == addr) {
            printf("Breakpoint already exists at 0x%x\n", addr);
            return;
        }
    }
    
    vm->breakpoints[vm->bp_count].addr = addr;
    vm->breakpoints[vm->bp_count].enabled = true;
    vm->bp_count++;
    printf("Breakpoint %d set at 0x%x\n", vm->bp_count, addr);
}

static void list_breakpoints(VM* vm) {
    printf("\nBreakpoints:\n");
    if (vm->bp_count == 0) {
        printf("  None\n");
        return;
    }
    
    for (int i = 0; i < vm->bp_count; i++) {
        printf("  %d: 0x%04x %s\n", i + 1, vm->breakpoints[i].addr,
               vm->breakpoints[i].enabled ? "[enabled]" : "[disabled]");
    }
}

static void delete_breakpoint(VM* vm, int num) {
    if (num < 1 || num > vm->bp_count) {
        printf("Invalid breakpoint number\n");
        return;
    }
    
    for (int i = num - 1; i < vm->bp_count - 1; i++) {
        vm->breakpoints[i] = vm->breakpoints[i + 1];
    }
    vm->bp_count--;
    printf("Breakpoint deleted\n");
}

static bool check_breakpoint(VM* vm) {
    for (int i = 0; i < vm->bp_count; i++) {
        if (vm->breakpoints[i].enabled && vm->breakpoints[i].addr == vm->pc) {
            printf("\n*** Breakpoint %d hit at 0x%x ***\n", i + 1, vm->pc);
            return true;
        }
    }
    return false;
}

// VM execution

static bool vm_step(VM* vm) {
    const uint8_t* code = vm->code;
    const uint32_t code_size = vm->code_size;
    uint32_t pc = vm->pc;

    if (pc >= code_size) {
        printf("PC out of code bounds\n");
        return false;
    }

    uint8_t op = code[pc++];
    int32_t imm = 0;
    if (has_imm32(op)) {
        imm = fetch_i32(code, code_size, &pc);
    }

    switch (op) {
        case OPAdd:  { int32_t b=pop32(vm), a=pop32(vm); push32(vm, a+b); } break;
        case OPNeg:  { int32_t a=pop32(vm); push32(vm, -a); } break;
        case OPMul:  { int32_t b=pop32(vm), a=pop32(vm); push32(vm, a*b); } break;
        case OPDivD: { int32_t b=pop32(vm), a=pop32(vm); if (b==0) btvm_die("div by 0"); push32(vm, a/b); } break;
        case OPRemD: { int32_t b=pop32(vm), a=pop32(vm); if (b==0) btvm_die("mod by 0"); push32(vm, a%b); } break;
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
            if (vm->sp + 8u > vm->stack_base + vm->stack_size) btvm_die("swap underflow");
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
            uint32_t addr = (uint32_t)pop32(vm);
            int32_t v = pop32(vm);
            mem_write32(vm->mem, vm->mem_size, addr, v);
        } break;

        case OPHalt: {
            int32_t code0 = mem_read32(vm->mem, vm->mem_size, vm->sp);
            printf("\n*** Program halted with exit code %d ***\n", code0);
            return false;
        }

        case OPWrI: {
            int32_t width = pop32(vm);
            int32_t value = pop32(vm);
            vm_print_int_width(value, width);
            fflush(stdout);
        } break;

        case OPWrC: {
            int32_t v = pop32(vm);
            putchar((unsigned char)(v & 0xFF));
            fflush(stdout);
        } break;

        case OPWrL: {
            putchar('\n');
            fflush(stdout);
        } break;

        case OPRdI: {
            uint32_t addr = (uint32_t)pop32(vm);
            int32_t x = 0;
            printf("\nInput integer: ");
            fflush(stdout);
            if (scanf("%d", &x) != 1) x = 0;
            mem_write32(vm->mem, vm->mem_size, addr, x);
        } break;

        case OPRdC: {
            uint32_t addr = (uint32_t)pop32(vm);
            printf("\nInput character: ");
            fflush(stdout);
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
            int64_t addr = (int64_t)vm->ebp + (int64_t)imm;
            if (addr < 0 || (uint64_t)addr >= vm->mem_size) btvm_die("OPLdA addr OOB");
            push32(vm, (int32_t)addr);
        } break;

        case OPLdLA: {
            int64_t addr = (int64_t)vm->sp + (int64_t)imm;
            if (addr < 0 || (uint64_t)addr >= vm->mem_size) btvm_die("OPLdLA addr OOB");
            push32(vm, (int32_t)addr);
        } break;

        case OPLdL: {
            int64_t addr = (int64_t)vm->sp + (int64_t)imm;
            if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) btvm_die("OPLdL addr OOB");
            int32_t v = mem_read32(vm->mem, vm->mem_size, (uint32_t)addr);
            push32(vm, v);
        } break;

        case OPLdG: {
            int64_t addr = (int64_t)vm->ebp + (int64_t)imm;
            if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) btvm_die("OPLdG addr OOB");
            int32_t v = mem_read32(vm->mem, vm->mem_size, (uint32_t)addr);
            push32(vm, v);
        } break;

        case OPStL: {
            int32_t v = pop32(vm);
            int64_t addr = (int64_t)vm->sp + (int64_t)imm - 4;
            if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) btvm_die("OPStL addr OOB");
            mem_write32(vm->mem, vm->mem_size, (uint32_t)addr, v);
        } break;

        case OPStG: {
            int32_t v = pop32(vm);
            int64_t addr = (int64_t)vm->ebp + (int64_t)imm;
            if (addr < 0 || (uint64_t)addr + 4u > vm->mem_size) btvm_die("OPStG addr OOB");
            mem_write32(vm->mem, vm->mem_size, (uint32_t)addr, v);
        } break;

        case OPMove: {
            uint32_t dst = (uint32_t)pop32(vm);
            uint32_t src = (uint32_t)pop32(vm);
            uint32_t n = (uint32_t)imm;
            if (src + n > vm->mem_size || dst + n > vm->mem_size) btvm_die("OPMove OOB");
            memmove(vm->mem + dst, vm->mem + src, n);
        } break;

        case OPCopy: {
            uint32_t src = (uint32_t)pop32(vm);
            uint32_t n = (uint32_t)imm;
            if (vm->sp < vm->stack_base + n) btvm_die("OPCopy stack overflow");
            vm->sp -= n;
            uint32_t dst = vm->sp;
            if (src + n > vm->mem_size || dst + n > vm->mem_size) btvm_die("OPCopy OOB");
            memmove(vm->mem + dst, vm->mem + src, n);
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
            int64_t npc = (int64_t)pc + (int64_t)imm;
            if (npc < 0 || (uint64_t)npc > code_size) btvm_die("OPJmp target OOB");
            pc = (uint32_t)npc;
        } break;

        case OPJZ: {
            int32_t cond = pop32(vm);
            if (cond == 0) {
                int64_t npc = (int64_t)pc + (int64_t)imm;
                if (npc < 0 || (uint64_t)npc > code_size) btvm_die("OPJZ target OOB");
                pc = (uint32_t)npc;
            }
        } break;

        case OPCall: {
            uint32_t id = (uint32_t)imm;
            if (id >= vm->func_count) btvm_die("OPCall bad func id");
            push32(vm, (int32_t)pc);
            pc = vm->funcs[id].code_off;
            if (pc >= code_size) btvm_die("OPCall callee pc OOB");
        } break;

        case OPAdjS: {
            int64_t nsp = (int64_t)vm->sp + (int64_t)imm;
            uint32_t lo = vm->stack_base;
            uint32_t hi = vm->stack_base + vm->stack_size;
            if (nsp < (int64_t)lo || nsp > (int64_t)hi) btvm_die("OPAdjS out of stack");
            vm->sp = (uint32_t)nsp;
        } break;

        case OPExit: {
            uint32_t ret_pc = (uint32_t)mem_read32(vm->mem, vm->mem_size, vm->sp);
            int64_t nsp = (int64_t)vm->sp + (int64_t)imm;
            uint32_t lo = vm->stack_base;
            uint32_t hi = vm->stack_base + vm->stack_size;
            if (nsp < (int64_t)lo || nsp > (int64_t)hi) btvm_die("OPExit bad sp");
            vm->sp = (uint32_t)nsp;
            if (ret_pc > code_size) btvm_die("OPExit bad return pc");
            pc = ret_pc;
        } break;

        default:
            btvm_die("unknown opcode");
    }

    vm->pc = pc;
    return true;
}

static void process_command(VM* vm, const char* cmd) {
    char c = cmd[0];
    
    if (c == 's' || c == '\n' || c == '\0') {
        vm->step_mode = true;
        if (!vm_step(vm)) {
            vm->running = false;
        }
    }
    else if (c == 'c') {
        vm->step_mode = false;
        vm->running = true;
    }
    else if (c == 'r') {
        show_registers(vm);
    }
    else if (c == 't') {
        int count = 8;
        if (sscanf(cmd, "t %d", &count) < 1) count = 8;
        show_stack(vm, count);
    }
    else if (c == 'm') {
        uint32_t addr = 0;
        int count = 16;
        if (sscanf(cmd, "m %x %d", &addr, &count) < 1) {
            printf("Usage: m <addr> [count]\n");
            return;
        }
        show_memory(vm, addr, count);
    }
    else if (c == 'b') {
        uint32_t addr = 0;
        if (sscanf(cmd, "b %x", &addr) == 1) {
            add_breakpoint(vm, addr);
        } else {
            printf("Usage: b <addr>\n");
        }
    }
    else if (c == 'l') {
        list_breakpoints(vm);
    }
    else if (c == 'd') {
        int num = 0;
        if (sscanf(cmd, "d %d", &num) == 1) {
            delete_breakpoint(vm, num);
        } else {
            printf("Usage: d <num>\n");
        }
    }
    else if (c == 'i') {
        uint32_t addr = vm->pc;
        int count = 10;
        if (sscanf(cmd, "i %x %d", &addr, &count) < 1) {
            addr = vm->pc;
        }
        printf("\nDisassembly at 0x%x:\n", addr);
        for (int i = 0; i < count; i++) {
            if (addr >= vm->code_size) break;
            addr = disassemble_one(vm, addr);
        }
    }
    else if (c == 'h' || c == '?') {
        printf("\nDebugger commands:\n");
        printf("  s, [enter]    - Step (execute one instruction)\n");
        printf("  c             - Continue execution\n");
        printf("  r             - Show registers\n");
        printf("  t [count]     - Show stack (default 8 values)\n");
        printf("  m <addr> [cnt]- Show memory at address\n");
        printf("  b <addr>      - Set breakpoint at address\n");
        printf("  l             - List breakpoints\n");
        printf("  d <num>       - Delete breakpoint\n");
        printf("  i [addr] [cnt]- Disassemble (default from PC, 10 instructions)\n");
        printf("  h, ?          - Show this help\n");
        printf("  q             - Quit\n");
    }
    else if (c == 'q') {
        printf("Exiting debugger.\n");
        exit(0);
    }
    else {
        printf("Unknown command. Type 'h' for help.\n");
    }
}

static void debug_loop(VM* vm) {
    char buf[256];
    
    printf("\nBTBC Debugger - type 'h' for help\n");
    show_current_instruction(vm);
    
    vm->running = true;
    vm->step_mode = true;
    
    while (vm->running) {
        if (vm->step_mode) {
            printf("\n(btdb) ");
            fflush(stdout);
            
            if (!fgets(buf, sizeof(buf), stdin)) break;
            
            size_t len = strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
            
            process_command(vm, buf);
            
            if (vm->running && vm->step_mode) {
                show_current_instruction(vm);
            }
        } else {
            if (check_breakpoint(vm)) {
                vm->step_mode = true;
                show_current_instruction(vm);
            } else {
                if (!vm_step(vm)) {
                    vm->running = false;
                }
            }
        }
    }
}

static void usage(void) {
    fprintf(stderr, "Usage: btdb [--stack <bytes>] file.btbc\n");
    exit(1);
}

int main(int argc, char** argv) {
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.stack_size = 8u * 1024u * 1024u; // default 8 MB

    const char* path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stack") == 0) {
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
    debug_loop(&vm);

    free(vm.funcs);
    free(vm.code);
    free(vm.mem);

    return 0;
}
