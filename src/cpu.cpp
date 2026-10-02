// ARM CPU (Unicorn) setup, trap hook and host->ARM calls.
#include "emu.h"
#include <cstdlib>

using namespace layout;

static const int kRegs[16] = {
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
    UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC};

uint32_t reg(int r) { uint32_t v = 0; uc_reg_read(g.uc, kRegs[r], &v); return v; }
void set_reg(int r, uint32_t v) { uc_reg_write(g.uc, kRegs[r], &v); }

static uint32_t cpsr() { uint32_t v = 0; uc_reg_read(g.uc, UC_ARM_REG_CPSR, &v); return v; }

void cpu_dump(const char* why) {
    LOG("---- CPU state (%s) ----", why);
    for (int i = 0; i < 16; i += 4)
        LOG("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, reg(i), i + 1, reg(i + 1), i + 2, reg(i + 2), i + 3, reg(i + 3));
    uint32_t pc = reg(15), cb = g.code_base;
    LOG("  cpsr=%08x  pc=code+%x lr=code+%x", cpsr(), pc - cb, reg(14) - cb);
}

// Trap page: every slot is "bx lr"; this hook runs the HLE handler right before it executes,
// so the handler only has to set r0 and the bx returns to the caller (ARM or Thumb).
static void hook_trap(uc_engine* uc, uint64_t address, uint32_t size, void*) {
    uint32_t id = (uint32_t)(address - TRAP_BASE) / 4;
    if (id == TRAP_RETURN) return;
    glue_dispatch(id);
    if (g.quit) uc_emu_stop(uc);
}

bool semihosting(uint32_t imm);   // misc.cpp

static void hook_intr(uc_engine* uc, uint32_t intno, void*) {
    uint32_t pc = reg(15);
    if (intno == 2) {
        uint32_t insn = r32(pc - 4);
        if (semihosting(insn & 0xFFFFFF)) return;
        LOG("!! SWI %06x at %08x", insn & 0xFFFFFF, pc - 4);
    } else {
        LOG("!! unexpected CPU exception %u", intno);
    }
    cpu_dump("exception");
    g.quit = true;
    uc_emu_stop(uc);
}

static bool hook_bad_mem(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void*) {
    const char* kind = type == UC_MEM_WRITE_UNMAPPED ? "write" : type == UC_MEM_FETCH_UNMAPPED ? "fetch" : "read";
    LOG("!! invalid %s at %08llx (size %d)", kind, (unsigned long long)addr, size);
    cpu_dump("bad memory");
    g.quit = true;
    return false;
}

// ARM920T ignores bits [1:0] of a BX target in ARM state (e.g. a "bx pc" Thumb veneer at an
// address = 2 mod 4). QEMU raises an undefined instruction instead: realign and continue.
static bool hook_invalid_insn(uc_engine* uc, void*) {
    uint32_t pc = reg(15), cpsr_v = cpsr();
    if (!(cpsr_v & 0x20) && (pc & 3)) {
        uint32_t fixed = pc & ~3u;
        uc_reg_write(uc, UC_ARM_REG_PC, &fixed);
        return true;
    }
    LOG("!! invalid instruction at %08x (code+%x)", pc, pc - g.code_base);
    cpu_dump("invalid instruction");
    g.quit = true;
    return false;
}

static void map(uint32_t base, uint32_t size, uint8_t** host) {
    *host = (uint8_t*)calloc(1, size);
    uc_err e = uc_mem_map_ptr(g.uc, base, size, UC_PROT_ALL, *host);
    if (e) { LOG("uc_mem_map_ptr(%08x): %s", base, uc_strerror(e)); exit(1); }
}

void cpu_init() {
    uc_err e = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &g.uc);
    if (e) { LOG("uc_open: %s", uc_strerror(e)); exit(1); }
    // ARMv5TE, pre-v6 unaligned semantics: closest to the i.MX1's ARM920T (ARMv4T).
    uc_ctl_set_cpu_model(g.uc, UC_CPU_ARM_926);
    // Palm OS 5 devices map low memory, so NULL dereferences read zeros instead of faulting;
    // several games rely on it.
    map(LOWMEM_BASE, LOWMEM_SIZE, &g.lowmem);
    map(STACK_BASE, STACK_SIZE, &g.stack);
    map(RAM_BASE, RAM_SIZE, &g.ram);
    map(VRAM_BASE, VRAM_SIZE, &g.vram);
    map(ROM_BASE, ROM_SIZE, &g.rom);
    map(TRAP_BASE, TRAP_SIZE, &g.trap);
    uc_hook h0, h1, h2;
    uc_hook_add(g.uc, &h0, UC_HOOK_CODE, (void*)hook_trap, nullptr, TRAP_BASE, TRAP_BASE + TRAP_SIZE - 1);
    uc_hook_add(g.uc, &h1, UC_HOOK_INTR, (void*)hook_intr, nullptr, 1, 0);
    uc_hook_add(g.uc, &h2, UC_HOOK_MEM_INVALID, (void*)hook_bad_mem, nullptr, 1, 0);
    uc_hook h3;
    uc_hook_add(g.uc, &h3, UC_HOOK_INSN_INVALID, (void*)hook_invalid_insn, nullptr, 1, 0);
    uint32_t sp = STACK_BASE + STACK_SIZE - 16;
    set_reg(13, sp);
}

// Call ARM code at addr (bit 0 = Thumb) with AAPCS arguments; returns r0.
// Re-entrant: may be called from inside an HLE handler (nested emulation).
uint32_t cpu_call(uint32_t addr, const std::vector<uint32_t>& args) {
    uint32_t saved[16], saved_cpsr = cpsr();
    for (int i = 0; i < 16; i++) saved[i] = reg(i);

    uint32_t sp = saved[13];
    size_t nstack = args.size() > 4 ? args.size() - 4 : 0;
    sp = (sp - 64 - (uint32_t)nstack * 4) & ~7u;
    for (size_t i = 0; i < nstack; i++) w32(sp + (uint32_t)i * 4, args[4 + i]);
    for (size_t i = 0; i < 4 && i < args.size(); i++) set_reg((int)i, args[i]);
    set_reg(13, sp);
    uint32_t ret_addr = TRAP_BASE + TRAP_RETURN * 4;
    set_reg(14, ret_addr);

    g.nest++;
    uc_err e = uc_emu_start(g.uc, addr, ret_addr, 0, 0);
    g.nest--;
    if (e) {
        LOG("!! emulation error: %s (calling %08x)", uc_strerror(e), addr);
        g.quit = true;
    }
    uint32_t r0 = reg(0);
    uc_reg_write(g.uc, UC_ARM_REG_CPSR, &saved_cpsr);
    for (int i = 0; i < 16; i++) set_reg(i, saved[i]);
    return r0;
}

// ---- basic-block ring buffer (debugging) --------------------------------------------
static std::vector<uint32_t> g_blocks(100000);
static size_t g_block_pos = 0;
static uint64_t g_block_count = 0;
static void hook_block(uc_engine*, uint64_t address, uint32_t, void*) {
    g_blocks[g_block_pos] = (uint32_t)address;
    g_block_pos = (g_block_pos + 1) % g_blocks.size();
    g_block_count++;
}
void cpu_trace_blocks(uint32_t base, uint32_t size) {
    uc_hook h;
    uc_hook_add(g.uc, &h, UC_HOOK_BLOCK, (void*)hook_block, nullptr, base, base + size - 1);
}
void cpu_dump_blocks(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    size_t n = g_block_count < g_blocks.size() ? (size_t)g_block_count : g_blocks.size();
    size_t start = (g_block_pos + g_blocks.size() - n) % g_blocks.size();
    uint32_t prev = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t a = g_blocks[(start + i) % g_blocks.size()] - g.code_base;
        if (a != prev) fprintf(f, "%x\n", a);
        prev = a;
    }
    fclose(f);
    LOG("wrote %zu block entries to %s", n, path.c_str());
}

// Thumb "bx pc" veneers placed at an address = 2 mod 4 jump to ARM state at a misaligned
// address; the ARM920T silently aligns it, QEMU does not. Redirect each such target.
static void hook_realign(uc_engine* uc, uint64_t address, uint32_t size, void*) {
    uint32_t cpsr_v = cpsr();
    if (cpsr_v & 0x20) return;   // reached in Thumb state: not the veneer path
    uint32_t fixed = (uint32_t)address & ~3u;
    uc_reg_write(uc, UC_ARM_REG_PC, &fixed);
}
void cpu_fix_bx_pc_veneers(uint32_t base, uint32_t size) {
    int n = 0;
    for (uint32_t off = 2; off + 6 <= size; off += 4) {
        if (r16(base + off) != 0x4778) continue;   // bx pc
        uint32_t target = base + off + 4;           // ARM state, bit 1 set
        uc_hook h;
        uc_hook_add(g.uc, &h, UC_HOOK_CODE, (void*)hook_realign, nullptr, target, target);
        n++;
    }
    if (n) LOG("patched %d misaligned Thumb 'bx pc' veneers", n);
}
