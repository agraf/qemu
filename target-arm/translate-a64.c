/*
 *  AArch64 translation
 *
 *  Copyright (c) 2013 Alexander Graf <agraf@suse.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "cpu.h"
#include "tcg-op.h"
#include "qemu/log.h"
#include "translate.h"
#include "qemu/host-utils.h"

#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"

static TCGv_i64 cpu_X[32];
static TCGv_i64 cpu_pc;
static TCGv_i32 pstate;

static const char *regnames[] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "lr", "sp"
};

/* initialize TCG globals.  */
void a64_translate_init(void)
{
    int i;

    cpu_pc = tcg_global_mem_new_i64(TCG_AREG0,
                                    offsetof(CPUARMState, pc),
                                    "pc");
    for (i = 0; i < 32; i++) {
        cpu_X[i] = tcg_global_mem_new_i64(TCG_AREG0,
                                          offsetof(CPUARMState, xregs[i]),
                                          regnames[i]);
    }

    pstate = tcg_global_mem_new_i32(TCG_AREG0,
                                    offsetof(CPUARMState, pstate),
                                    "pstate");
}

void aarch64_cpu_dump_state(CPUState *cs, FILE *f,
                            fprintf_function cpu_fprintf, int flags)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    int i;

    cpu_fprintf(f, "PC=%016"PRIx64"  SP=%016"PRIx64"\n",
            env->pc, env->xregs[31]);
    for (i = 0; i < 31; i++) {
        cpu_fprintf(f, "X%02d=%016"PRIx64, i, env->xregs[i]);
        if ((i % 4) == 3) {
            cpu_fprintf(f, "\n");
        } else {
            cpu_fprintf(f, " ");
        }
    }
    cpu_fprintf(f, "PSTATE=%c%c%c%c\n",
        env->pstate & PSTATE_N ? 'n' : '.',
        env->pstate & PSTATE_Z ? 'z' : '.',
        env->pstate & PSTATE_C ? 'c' : '.',
        env->pstate & PSTATE_V ? 'v' : '.');
    cpu_fprintf(f, "\n");

    if (flags & CPU_DUMP_FPU) {
        int numvfpregs = 32;
        for (i = 0; i < numvfpregs; i++) {
            uint64_t v = float64_val(env->vfp.regs[i * 2]);
            uint64_t v1 = float64_val(env->vfp.regs[(i * 2) + 1]);
            if (!v && !v1) {
                /* skip empty registers - makes traces easier to read */
                continue;
            }
            cpu_fprintf(f, "d%02d.0=%016" PRIx64 " " "d%02d.0=%016" PRIx64 "\n",
                        i, v, i, v1);
        }
        cpu_fprintf(f, "FPSCR: %08x\n", (int)env->vfp.xregs[ARM_VFP_FPSCR]);
    }
}

static int get_mem_index(DisasContext *s)
{
    /* XXX only user mode for now */
    return 1;
}

void gen_a64_set_pc_im(uint64_t val)
{
    tcg_gen_movi_i64(cpu_pc, val);
}

static void gen_exception(int excp)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    tcg_gen_movi_i32(tmp, excp);
    gen_helper_exception(cpu_env, tmp);
    tcg_temp_free_i32(tmp);
}

static void gen_exception_insn(DisasContext *s, int offset, int excp)
{
    gen_a64_set_pc_im(s->pc - offset);
    gen_exception(excp);
    s->is_jmp = DISAS_JUMP;
}

static void real_unallocated_encoding(DisasContext *s)
{
    fprintf(stderr, "Unknown instruction: %#x\n", s->insn);
    gen_exception_insn(s, 4, EXCP_UDEF);
}

#define unallocated_encoding(s) do { \
    fprintf(stderr, "unallocated encoding at line: %d\n", __LINE__); \
    real_unallocated_encoding(s); \
    } while (0)


static TCGv_i64 cpu_reg(int reg)
{
    if (reg == 31) {
        /* XXX leaks temps */
        return tcg_const_i64(0);
    } else {
        return cpu_X[reg];
    }
}

static TCGv_i64 cpu_reg_sp(int reg)
{
    return cpu_X[reg];
}

static void clear_fpreg(int dest)
{
    int freg_offs = offsetof(CPUARMState, vfp.regs[dest * 2]);
    TCGv_i64 tcg_zero = tcg_const_i64(0);

    tcg_gen_st_i64(tcg_zero, cpu_env, freg_offs);
    tcg_gen_st_i64(tcg_zero, cpu_env, freg_offs + sizeof(float64));
}

static TCGv_ptr get_fpstatus_ptr(void)
{
    TCGv_ptr statusptr = tcg_temp_new_ptr();
    int offset;

    offset = offsetof(CPUARMState, vfp.standard_fp_status);
    tcg_gen_addi_ptr(statusptr, cpu_env, offset);

    return statusptr;
}

static inline bool use_goto_tb(DisasContext *s, int n, uint64_t dest)
{
    /* No direct tb linking with singlestep or deterministic io */
    if (s->singlestep_enabled || (s->tb->cflags & CF_LAST_IO)) {
        return false;
    }

    /* Only link tbs from inside the same guest page */
    if ((s->tb->pc & TARGET_PAGE_MASK) != (dest & TARGET_PAGE_MASK)) {
        return false;
    }

    return true;
}

static inline void gen_goto_tb(DisasContext *s, int n, uint64_t dest)
{
    TranslationBlock *tb;

    tb = s->tb;
    if (use_goto_tb(s, n, dest)) {
        tcg_gen_goto_tb(n);
        gen_a64_set_pc_im(dest);
        tcg_gen_exit_tb((tcg_target_long)tb + n);
        s->is_jmp = DISAS_TB_JUMP;
    } else {
        gen_a64_set_pc_im(dest);
        tcg_gen_exit_tb(0);
        s->is_jmp = DISAS_JUMP;
    }
}

static void handle_b(DisasContext *s, uint32_t insn)
{
    uint64_t addr = s->pc - 4 + (sextract32(insn, 0, 26) << 2);

    if (extract32(insn, 31, 1)) {
        /* BL */
        tcg_gen_movi_i64(cpu_reg(30), s->pc);
    }
    gen_goto_tb(s, 0, addr);
}

static void handle_br(DisasContext *s, uint32_t insn)
{
    int branch_type = extract32(insn, 21, 2);
    int source = extract32(insn, 5, 5);

    switch (branch_type) {
    case 0: /* JMP */
    case 2: /* RET */
        break;
    case 1: /* CALL */
        tcg_gen_movi_i64(cpu_reg(30), s->pc);
        break;
    case 3:
        unallocated_encoding(s);
        return;
    }

    tcg_gen_mov_i64(cpu_pc, cpu_reg(source));
    s->is_jmp = DISAS_JUMP;
}

static void handle_cb(DisasContext *s, uint32_t insn)
{
    uint64_t addr = s->pc - 4 + (sextract32(insn, 5, 19) << 2);
    bool is_zero = !extract32(insn, 24, 1);
    bool is_32bit = !extract32(insn, 31, 1);
    int dest = extract32(insn, 0, 5);
    int no_match;
    TCGv_i64 tcg_cmp, tcg_zero;

    tcg_cmp = tcg_temp_new_i64();
    tcg_zero = tcg_const_i64(0);

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_cmp, cpu_reg(dest));
    } else {
        tcg_gen_mov_i64(tcg_cmp, cpu_reg(dest));
    }

    no_match = gen_new_label();
    if (is_zero) {
        tcg_gen_brcond_i64(TCG_COND_NE, tcg_cmp, tcg_zero, no_match);
    } else {
        tcg_gen_brcond_i64(TCG_COND_EQ, tcg_cmp, tcg_zero, no_match);
    }
    gen_goto_tb(s, 0, addr);

    gen_set_label(no_match);
    gen_goto_tb(s, 1, s->pc);

    tcg_temp_free_i64(tcg_cmp);
    tcg_temp_free_i64(tcg_zero);
}

static void handle_condb(DisasContext *s, uint32_t insn)
{
    uint64_t addr = s->pc - 4 + (sextract32(insn, 5, 19) << 2);
    int cond = extract32(insn, 0, 4);
    int no_match;
    TCGv_i32 tcg_zero = tcg_const_i32(0);
    TCGv_i32 tcg_cond = tcg_const_i32(cond);
    TCGv_i32 tcg_condmatch = tcg_temp_new_i32();

    no_match = gen_new_label();

    gen_helper_cond(tcg_condmatch, pstate, tcg_cond);
    tcg_gen_brcond_i32(TCG_COND_EQ, tcg_condmatch, tcg_zero, no_match);

    gen_goto_tb(s, 0, addr);

    gen_set_label(no_match);
    gen_goto_tb(s, 1, s->pc);

    tcg_temp_free_i32(tcg_zero);
    tcg_temp_free_i32(tcg_cond);
    tcg_temp_free_i32(tcg_condmatch);
}

static void handle_tbz(DisasContext *s, uint32_t insn)
{
    uint64_t addr = s->pc - 4 + (sextract32(insn, 5, 14) << 2);
    bool is_one = extract32(insn, 24, 1);
    int shift = extract32(insn, 19, 5) | (extract32(insn, 31, 1) << 5);
    int source = extract32(insn, 0, 5);
    int no_match;
    uint64_t mask = 1ULL << shift;
    TCGv_i64 tcg_cmp, tcg_mask;

    tcg_cmp = tcg_temp_new_i64();
    tcg_mask = tcg_const_i64(mask);
    tcg_gen_and_i64(tcg_cmp, cpu_reg(source), tcg_mask);

    no_match = gen_new_label();
    if (is_one) {
        tcg_gen_brcond_i64(TCG_COND_NE, tcg_cmp, tcg_mask, no_match);
    } else {
        tcg_gen_brcond_i64(TCG_COND_EQ, tcg_cmp, tcg_mask, no_match);
    }
    gen_goto_tb(s, 0, addr);
    tcg_gen_exit_tb(0);

    gen_set_label(no_match);
    gen_goto_tb(s, 1, s->pc);

    tcg_temp_free_i64(tcg_cmp);
    tcg_temp_free_i64(tcg_mask);
}

static void ldst_do_vec_int(DisasContext *s, int freg_offs, TCGv_i64 tcg_addr,
                            int size, bool is_store)
{
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();

    if (is_store) {
        switch (size) {
        case 0:
            tcg_gen_ld8u_i64(tcg_tmp, cpu_env, freg_offs);
            tcg_gen_qemu_st8(tcg_tmp, tcg_addr, get_mem_index(s));
            break;
        case 1:
            tcg_gen_ld16u_i64(tcg_tmp, cpu_env, freg_offs);
            tcg_gen_qemu_st16(tcg_tmp, tcg_addr, get_mem_index(s));
            break;
        case 2:
            tcg_gen_ld32u_i64(tcg_tmp, cpu_env, freg_offs);
            tcg_gen_qemu_st32(tcg_tmp, tcg_addr, get_mem_index(s));
            break;
        case 4:
            tcg_gen_ld_i64(tcg_tmp, cpu_env, freg_offs);
            tcg_gen_qemu_st64(tcg_tmp, tcg_addr, get_mem_index(s));
            freg_offs += sizeof(uint64_t);
            tcg_gen_addi_i64(tcg_addr, tcg_addr, sizeof(uint64_t));
            /* fall through */
        case 3:
            tcg_gen_ld_i64(tcg_tmp, cpu_env, freg_offs);
            tcg_gen_qemu_st64(tcg_tmp, tcg_addr, get_mem_index(s));
            break;
        }
    } else {
        switch (size) {
        case 0:
            tcg_gen_qemu_ld8u(tcg_tmp, tcg_addr, get_mem_index(s));
            tcg_gen_st8_i64(tcg_tmp, cpu_env, freg_offs);
            break;
        case 1:
            tcg_gen_qemu_ld16u(tcg_tmp, tcg_addr, get_mem_index(s));
            tcg_gen_st16_i64(tcg_tmp, cpu_env, freg_offs);
            break;
        case 2:
            tcg_gen_qemu_ld32u(tcg_tmp, tcg_addr, get_mem_index(s));
            tcg_gen_st32_i64(tcg_tmp, cpu_env, freg_offs);
            break;
        case 4:
            tcg_gen_qemu_ld64(tcg_tmp, tcg_addr, get_mem_index(s));
            tcg_gen_st_i64(tcg_tmp, cpu_env, freg_offs);
            freg_offs += sizeof(uint64_t);
            tcg_gen_addi_i64(tcg_addr, tcg_addr, sizeof(uint64_t));
            /* fall through */
        case 3:
            tcg_gen_qemu_ld64(tcg_tmp, tcg_addr, get_mem_index(s));
            tcg_gen_st_i64(tcg_tmp, cpu_env, freg_offs);
            break;
        }
    }

    tcg_temp_free_i64(tcg_tmp);
}

static void ldst_do_vec(DisasContext *s, int dest, TCGv_i64 tcg_addr_real,
                        int size, bool is_store)
{
    TCGv_i64 tcg_addr = tcg_temp_new_i64();
    int freg_offs = offsetof(CPUARMState, vfp.regs[dest * 2]);

    /* we don't want to modify the caller's tcg_addr */
    tcg_gen_mov_i64(tcg_addr, tcg_addr_real);

    if (!is_store) {
        /* normal ldst clears non-loaded bits */
        clear_fpreg(dest);
    }

    ldst_do_vec_int(s, freg_offs, tcg_addr, size, is_store);

    tcg_temp_free(tcg_addr);
}

static void ldst_do_gpr(DisasContext *s, int dest, TCGv_i64 tcg_addr, int size,
                        bool is_store, bool is_signed)
{
    if (is_store) {
        switch (size) {
        case 0:
            tcg_gen_qemu_st8(cpu_reg(dest), tcg_addr, get_mem_index(s));
            break;
        case 1:
            tcg_gen_qemu_st16(cpu_reg(dest), tcg_addr, get_mem_index(s));
            break;
        case 2:
            tcg_gen_qemu_st32(cpu_reg(dest), tcg_addr, get_mem_index(s));
            break;
        case 3:
            tcg_gen_qemu_st64(cpu_reg(dest), tcg_addr, get_mem_index(s));
            break;
        }
    } else {
        if (is_signed) {
            /* XXX check what impact regsize has */
            switch (size) {
            case 0:
                tcg_gen_qemu_ld8s(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            case 1:
                tcg_gen_qemu_ld16s(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            case 2:
                tcg_gen_qemu_ld32s(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            case 3:
                tcg_gen_qemu_ld64(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            }
        } else {
            switch (size) {
            case 0:
                tcg_gen_qemu_ld8u(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            case 1:
                tcg_gen_qemu_ld16u(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            case 2:
                tcg_gen_qemu_ld32u(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            case 3:
                tcg_gen_qemu_ld64(cpu_reg(dest), tcg_addr, get_mem_index(s));
                break;
            }
        }
    }
}

static void ldst_do(DisasContext *s, int dest, TCGv_i64 tcg_addr, int size,
                    bool is_store, bool is_signed, bool is_vector)
{
    if (is_vector) {
        ldst_do_vec(s, dest, tcg_addr, size, is_store);
    } else {
        ldst_do_gpr(s, dest, tcg_addr, size, is_store, is_signed);
    }
}

static void handle_stp(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rt2 = extract32(insn, 10, 5);
    int offset = sextract32(insn, 15, 7);
    int is_store = !extract32(insn, 22, 1);
    int type = extract32(insn, 23, 2);
    int is_vector = extract32(insn, 26, 1);
    int is_signed = extract32(insn, 30, 1);
    int is_32bit = !extract32(insn, 31, 1);
    TCGv_i64 tcg_addr;
    bool postindex;
    bool wback;
    int size = is_32bit ? 2 : 3;

    if (is_vector) {
        size = 2 + extract32(insn, 30, 2);
    }

    switch (type) {
    default:
    case 0:
        postindex = false;
        wback = false;
        break;
    case 1: /* STP (post-index) */
        postindex = true;
        wback = true;
        break;
    case 2: /* STP (signed offset */
        postindex = false;
        wback = false;
        break;
    case 3: /* STP (pre-index) */
        postindex = false;
        wback = true;
        break;
    }

    if (is_signed && !is_32bit) {
        unallocated_encoding(s);
        return;
    }

    offset <<= size;

    tcg_addr = tcg_temp_new_i64();
    if (rn == 31) {
        /* XXX check SP alignment */
    }
    tcg_gen_mov_i64(tcg_addr, cpu_reg_sp(rn));

    if (!postindex) {
        tcg_gen_addi_i64(tcg_addr, tcg_addr, offset);
    }

    ldst_do(s, rt, tcg_addr, size, is_store, is_signed, is_vector);
    tcg_gen_addi_i64(tcg_addr, tcg_addr, 1 << size);
    ldst_do(s, rt2, tcg_addr, size, is_store, is_signed, is_vector);
    tcg_gen_subi_i64(tcg_addr, tcg_addr, 1 << size);

    if (wback) {
        if (postindex) {
            tcg_gen_addi_i64(tcg_addr, tcg_addr, offset);
        }
        tcg_gen_mov_i64(cpu_reg_sp(rn), tcg_addr);
    }

    tcg_temp_free_i64(tcg_addr);
}

static void handle_ldarx(DisasContext *s, uint32_t insn)
{
    int rt = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rt2 = extract32(insn, 10, 5);
    int is_atomic = !extract32(insn, 15, 1);
    int rs = extract32(insn, 16, 5);
    int is_pair = extract32(insn, 21, 1);
    int is_store = !extract32(insn, 22, 1);
    int is_excl = !extract32(insn, 23, 1);
    int size = extract32(insn, 30, 2);
    TCGv_i64 tcg_addr;

    tcg_addr = tcg_temp_new_i64();
    if (rn == 31) {
        /* XXX check SP alignment */
    }
    tcg_gen_mov_i64(tcg_addr, cpu_reg_sp(rn));

    if (is_atomic) {
        /* XXX add locking */
    }
    if (is_store && is_excl) {
        /* XXX find out what status it wants */
        tcg_gen_movi_i64(cpu_reg(rs), 0);
    }

    ldst_do_gpr(s, rt, tcg_addr, size, is_store, false);
    if (is_pair) {
        tcg_gen_addi_i64(tcg_addr, tcg_addr, 1 << size);
        ldst_do_gpr(s, rt2, tcg_addr, size, is_store, false);
    }

    tcg_temp_free_i64(tcg_addr);
}

static TCGv_i64 get_shift(int reg, int shift_type, TCGv_i64 tcg_shift,
                          int is_32bit)
{
    TCGv_i64 r;

    r = tcg_temp_new_i64();

    /* XXX carry_out */
    switch (shift_type) {
    case 0: /* LSL */
        tcg_gen_shl_i64(r, cpu_reg(reg), tcg_shift);
        break;
    case 1: /* LSR */
        tcg_gen_shr_i64(r, cpu_reg(reg), tcg_shift);
        break;
    case 2: /* ASR */
        if (is_32bit) {
            TCGv_i64 tcg_tmp = tcg_temp_new_i64();
            tcg_gen_ext32s_i64(tcg_tmp, cpu_reg(reg));
            tcg_gen_sar_i64(r, tcg_tmp, tcg_shift);
            tcg_temp_free_i64(tcg_tmp);
        } else {
            tcg_gen_sar_i64(r, cpu_reg(reg), tcg_shift);
        }
        break;
    case 3:
        tcg_gen_rotr_i64(r, cpu_reg(reg), tcg_shift);
        break;
    }

    return r;
}

static TCGv_i64 get_shifti(int reg, int shift_type, int shift, int is_32bit)
{
    TCGv_i64 tcg_shift;
    TCGv_i64 r;

    if (!shift) {
        r = tcg_temp_new_i64();
        tcg_gen_mov_i64(r, cpu_reg(reg));
        return r;
    }

    tcg_shift = tcg_const_i64(shift);
    r = get_shift(reg, shift_type, tcg_shift, is_32bit);
    tcg_temp_free_i64(tcg_shift);

    return r;
}

static void handle_orr(DisasContext *s, uint32_t insn)
{
    int is_32bit = !extract32(insn, 31, 1);
    int dest = extract32(insn, 0, 5);
    int source = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    int shift_amount = sextract32(insn, 10, 6);
    int is_n = extract32(insn, 21, 1);
    int shift_type = extract32(insn, 22, 2);
    int opc = extract32(insn, 29, 2);
    bool setflags = (opc == 0x3);
    TCGv_i64 tcg_op2;
    TCGv_i64 tcg_dest;

    if (is_32bit && (shift_amount < 0)) {
        /* reserved value */
        unallocated_encoding(s);
    }

    /* MOV is dest = xzr & (source & ~0) */
    if (!shift_amount && source == 0x1f) {
        if (is_32bit) {
            tcg_gen_ext32u_i64(cpu_reg_sp(dest), cpu_reg(rm));
        } else {
            tcg_gen_mov_i64(cpu_reg_sp(dest), cpu_reg(rm));
        }
        if (is_n) {
            tcg_gen_not_i64(cpu_reg_sp(dest), cpu_reg_sp(dest));
        }
        if (is_32bit) {
            tcg_gen_ext32u_i64(cpu_reg_sp(dest), cpu_reg_sp(dest));
        }
        return;
    }

    tcg_op2 = get_shifti(rm, shift_type, shift_amount & (is_32bit ? 31 : 63),
                         is_32bit);
    if (is_n) {
        tcg_gen_not_i64(tcg_op2, tcg_op2);
    }

    tcg_dest = cpu_reg(dest);
    switch (opc) {
    case 0x0:
    case 0x3:
        tcg_gen_and_i64(tcg_dest, cpu_reg(source), tcg_op2);
        break;
    case 0x1:
        tcg_gen_or_i64(tcg_dest, cpu_reg(source), tcg_op2);
        break;
    case 0x2:
        tcg_gen_xor_i64(tcg_dest, cpu_reg(source), tcg_op2);
        break;
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_dest, tcg_dest);
    }

    if (setflags) {
        gen_helper_pstate_add(pstate, pstate, tcg_dest, cpu_reg(31), tcg_dest);
    }

    tcg_temp_free_i64(tcg_op2);
}

static void setflags_add(bool sub_op, bool is_32bit, TCGv_i64 src,
                         TCGv_i64 op2, TCGv_i64 res)
{
    if (sub_op) {
        if (is_32bit) {
            gen_helper_pstate_sub32(pstate, pstate, src, op2, res);
        } else {
            gen_helper_pstate_sub(pstate, pstate, src, op2, res);
        }
    } else {
        if (is_32bit) {
            gen_helper_pstate_add32(pstate, pstate, src, op2, res);
        } else {
            gen_helper_pstate_add(pstate, pstate, src, op2, res);
        }
    }
}

static void reg_extend(TCGv_i64 tcg_offset, int option, int shift, int reg)
{
    int extsize = extract32(option, 0, 2);
    bool is_signed = extract32(option, 2, 1);

    if (is_signed) {
        switch (extsize) {
        case 0:
            tcg_gen_ext8s_i64(tcg_offset, cpu_reg(reg));
            break;
        case 1:
            tcg_gen_ext16s_i64(tcg_offset, cpu_reg(reg));
            break;
        case 2:
            tcg_gen_ext32s_i64(tcg_offset, cpu_reg(reg));
            break;
        case 3:
            tcg_gen_mov_i64(tcg_offset, cpu_reg(reg));
            break;
        }
    } else {
        switch (extsize) {
        case 0:
            tcg_gen_ext8u_i64(tcg_offset, cpu_reg(reg));
            break;
        case 1:
            tcg_gen_ext16u_i64(tcg_offset, cpu_reg(reg));
            break;
        case 2:
            tcg_gen_ext32u_i64(tcg_offset, cpu_reg(reg));
            break;
        case 3:
            tcg_gen_mov_i64(tcg_offset, cpu_reg(reg));
            break;
        }
    }

    if (shift) {
        tcg_gen_shli_i64(tcg_offset, tcg_offset, shift);
    }
}

static void handle_add(DisasContext *s, uint32_t insn)
{
    int dest = extract32(insn, 0, 5);
    int source = extract32(insn, 5, 5);
    int shift_amount = sextract32(insn, 10, 6);
    int rm = extract32(insn, 16, 5);
    bool extend = extract32(insn, 21, 1);
    int shift_type = extract32(insn, 22, 2);
    bool is_carry = (extract32(insn, 24, 5) == 0x1a);
    bool setflags = extract32(insn, 29, 1);
    bool sub_op = extract32(insn, 30, 1);
    bool is_32bit = !extract32(insn, 31, 1);
    TCGv_i64 tcg_op2;
    TCGv_i64 tcg_src = tcg_temp_new_i64();
    TCGv_i64 tcg_dst;
    TCGv_i64 tcg_result = tcg_temp_new_i64();

    if (extend && shift_type) {
        unallocated_encoding(s);
    }

    tcg_gen_mov_i64(tcg_src, cpu_reg(source));
    tcg_dst = cpu_reg(dest);
    if (extend) {
        if ((shift_amount & 0x7) > 4) {
            /* reserved value */
            unallocated_encoding(s);
        }
        if (!setflags) {
            tcg_gen_mov_i64(tcg_src, cpu_reg_sp(source));
            tcg_dst = cpu_reg_sp(dest);
        }
    } else {
        if (shift_type == 3) {
            /* reserved value */
            unallocated_encoding(s);
        }
        if (is_32bit && (shift_amount < 0)) {
            /* reserved value */
            unallocated_encoding(s);
        }
    }

    if (extend) {
        tcg_op2 = tcg_temp_new_i64();
        reg_extend(tcg_op2, shift_amount >> 3, shift_amount & 0x7, rm);
    } else {
        tcg_op2 = get_shifti(rm, shift_type, shift_amount, is_32bit);
    }

    if (is_32bit) {
        tcg_gen_ext32s_i64(tcg_src, tcg_src);
        tcg_gen_ext32s_i64(tcg_op2, tcg_op2);
    }

    if (sub_op) {
        tcg_gen_sub_i64(tcg_result, tcg_src, tcg_op2);
    } else {
        tcg_gen_add_i64(tcg_result, tcg_src, tcg_op2);
    }

    if (is_carry) {
        TCGv_i64 tcg_carry = tcg_temp_new_i64();
        tcg_gen_extu_i32_i64(tcg_carry, pstate);
        tcg_gen_shri_i64(tcg_carry, tcg_carry, PSTATE_C_SHIFT);
        tcg_gen_andi_i64(tcg_carry, tcg_carry, 1);
        tcg_gen_add_i64(tcg_result, tcg_result, tcg_carry);
        if (sub_op) {
            tcg_gen_subi_i64(tcg_result, tcg_result, 1);
        }
        tcg_temp_free_i64(tcg_carry);
    }

    if (setflags) {
        setflags_add(sub_op, is_32bit, tcg_src, tcg_op2, tcg_result);
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_dst, tcg_result);
    } else {
        tcg_gen_mov_i64(tcg_dst, tcg_result);
    }

    tcg_temp_free_i64(tcg_src);
    tcg_temp_free_i64(tcg_op2);
    tcg_temp_free_i64(tcg_result);
}

/* SIMD load/store multiple (post-indexed) */
static void handle_simdldstm(DisasContext *s, uint32_t insn, bool is_wback)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    int size = extract32(insn, 10, 2);
    int opcode = extract32(insn, 12, 4);
    bool is_store = !extract32(insn, 22, 1);
    bool is_q = extract32(insn, 30, 1);
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    TCGv_i64 tcg_addr = tcg_temp_new_i64();
    int r, e, xs, tt, rpt, selem;
    int ebytes = 1 << size;
    int elements = (is_q ? 128 : 64) / (8 << size);

    tcg_gen_mov_i64(tcg_addr, cpu_reg_sp(rn));

    switch (opcode) {
    case 0x0:
        rpt = 1;
        selem = 4;
        break;
    case 0x2:
        rpt = 4;
        selem = 1;
        break;
    case 0x4:
        rpt = 1;
        selem = 3;
        break;
    case 0x6:
        rpt = 3;
        selem = 1;
        break;
    case 0x7:
        rpt = 1;
        selem = 1;
        break;
    case 0x8:
        rpt = 1;
        selem = 2;
        break;
    case 0xa:
        rpt = 2;
        selem = 1;
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    if (size == 3 && !is_q && selem != 1) {
        /* reserved */
        unallocated_encoding(s);
    }

    /* XXX check SP alignment on Rn */

    for (r = 0; r < rpt; r++) {
        for (e = 0; e < elements; e++) {
            tt = (rd + r) % 32;
            for (xs = 0; xs < selem; xs++) {
                int freg_offs = offsetof(CPUARMState, vfp.regs[tt * 2]) +
                                  (e * ebytes);

                ldst_do_vec_int(s, freg_offs, tcg_addr, size, is_store);
                tcg_gen_addi_i64(tcg_addr, tcg_addr, ebytes);
                tt = (tt + 1) % 32;
            }
        }
    }

    if (is_wback) {
        if (rm == 31) {
            tcg_gen_mov_i64(cpu_reg_sp(rn), tcg_addr);
        } else {
            tcg_gen_add_i64(cpu_reg_sp(rn), cpu_reg(rn), cpu_reg(rm));
        }
    }

    tcg_temp_free_i64(tcg_tmp);
    tcg_temp_free_i64(tcg_addr);
}

static void simd_ld(TCGv_i64 tcg_reg, int freg_offs, int size)
{
    switch (size) {
    case 0:
        tcg_gen_ld8u_i64(tcg_reg, cpu_env, freg_offs);
        break;
    case 1:
        tcg_gen_ld16u_i64(tcg_reg, cpu_env, freg_offs);
        break;
    case 2:
        tcg_gen_ld32u_i64(tcg_reg, cpu_env, freg_offs);
        break;
    case 3:
        tcg_gen_ld_i64(tcg_reg, cpu_env, freg_offs);
        break;
    }
}

static void simd_st(TCGv_i64 tcg_reg, int freg_offs, int size)
{
    switch (size) {
    case 0:
        tcg_gen_st8_i64(tcg_reg, cpu_env, freg_offs);
        break;
    case 1:
        tcg_gen_st16_i64(tcg_reg, cpu_env, freg_offs);
        break;
    case 2:
        tcg_gen_st32_i64(tcg_reg, cpu_env, freg_offs);
        break;
    case 3:
        tcg_gen_st_i64(tcg_reg, cpu_env, freg_offs);
        break;
    }
}

static void handle_dupg(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm5 = extract32(insn, 16, 6);
    int q = extract32(insn, 30, 1);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    int size;
    int i;

    for (size = 0; !(imm5 & (1 << size)); size++) {
        if (size > 3) {
            unallocated_encoding(s);
            return;
        }
    }

    if ((size == 3) && !q) {
        /* XXX reserved value */
        unallocated_encoding(s);
    }

    clear_fpreg(rd);
    for (i = 0; i < (q ? 16 : 8); i += (1 << size)) {
        simd_st(cpu_reg(rn), freg_offs_d + i, size);
    }
}

static void handle_umov(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm5 = extract32(insn, 16, 6);
    int q = extract32(insn, 30, 1);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int size;
    int idx;

    for (size = 0; !(imm5 & (1 << size)); size++) {
        if (size > 3) {
            unallocated_encoding(s);
            return;
        }
    }

    if ((size == 3) && !q) {
        /* XXX reserved value */
        unallocated_encoding(s);
    }

    idx = extract32(imm5, 1 + size, 4 - size) << size;
    simd_ld(cpu_reg(rd), freg_offs_n + idx, size);
}

static void handle_insg(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imm5 = extract32(insn, 16, 6);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    int size;
    int idx;

    for (size = 0; !(imm5 & (1 << size)); size++) {
        if (size > 3) {
            unallocated_encoding(s);
            return;
        }
    }

    idx = extract32(imm5, 1 + size, 4 - size) << size;
    simd_st(cpu_reg(rn), freg_offs_d + idx, size);
}

/* PC relative address calculation */
static void handle_adr(DisasContext *s, uint32_t insn)
{
    int reg = extract32(insn, 0, 5);
    int is_page = extract32(insn, 31, 1);
    uint64_t imm;
    uint64_t base;

    imm = sextract32(insn, 5, 19) << 2;
    imm |= extract32(insn, 29, 2);

    base = s->pc - 4;
    if (is_page) {
        /* ADRP (page based) */
        base &= ~0xFFFULL;
        imm <<= 12;
    }

    tcg_gen_movi_i64(cpu_reg(reg), base + imm);
}

static void handle_addi(DisasContext *s, uint32_t insn)
{
    TCGv_i64 tcg_result = tcg_temp_new_i64();
    TCGv_i64 tcg_imm;
    int dest = extract32(insn, 0, 5);
    int source = extract32(insn, 5, 5);
    uint64_t imm = extract32(insn, 10, 12);
    int shift = extract32(insn, 22, 2);
    bool setflags = extract32(insn, 29, 1);
    bool sub_op = extract32(insn, 30, 1);
    bool is_32bit = !extract32(insn, 31, 1);

    switch (shift) {
    case 0x0:
        break;
    case 0x1:
        imm <<= 12;
        break;
    default:
        unallocated_encoding(s);
    }

    tcg_imm = tcg_const_i64(imm);

    if (sub_op) {
        tcg_gen_subi_i64(tcg_result, cpu_reg_sp(source), imm);
    } else {
        tcg_gen_addi_i64(tcg_result, cpu_reg_sp(source), imm);
    }

    if (setflags) {
        setflags_add(sub_op, is_32bit, cpu_reg_sp(source), tcg_imm, tcg_result);
        if (is_32bit) {
            tcg_gen_ext32u_i64(cpu_reg(dest), tcg_result);
        } else {
            tcg_gen_mov_i64(cpu_reg(dest), tcg_result);
        }
    } else {
        if (is_32bit) {
            tcg_gen_ext32u_i64(cpu_reg_sp(dest), tcg_result);
        } else {
            tcg_gen_mov_i64(cpu_reg_sp(dest), tcg_result);
        }
    }

}

static void handle_movi(DisasContext *s, uint32_t insn)
{
    int reg = extract32(insn, 0, 5);
    uint64_t imm = extract32(insn, 5, 16);
    int is_32bit = !extract32(insn, 31, 1);
    int is_k = extract32(insn, 29, 1);
    int is_n = !extract32(insn, 30, 1);
    int pos = extract32(insn, 21, 2) << 4;
    TCGv_i64 tcg_imm;

    if (extract32(insn, 23, 1) != 1) {
        /* reserved */
        unallocated_encoding(s);
        return;
    }

    if (is_k && is_n) {
        unallocated_encoding(s);
        return;
    }

    if (is_k) {
        tcg_imm = tcg_const_i64(imm);
        tcg_gen_deposit_i64(cpu_reg(reg), cpu_reg(reg), tcg_imm, pos, 16);
        tcg_temp_free_i64(tcg_imm);
    } else {
        tcg_gen_movi_i64(cpu_reg(reg), imm << pos);
    }

    if (is_n) {
        tcg_gen_not_i64(cpu_reg(reg), cpu_reg(reg));
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(cpu_reg(reg), cpu_reg(reg));
    }
}

static uint64_t replicate(uint64_t mask, int esize)
{
    int i;
    uint64_t out_mask = 0;
    for (i = 0; (i * esize) < 64; i++) {
        out_mask = out_mask | (mask << (i * esize));
    }
    return out_mask;
}

static uint64_t bitmask(int len)
{
    if (len == 64) {
        return -1;
    }
    return (1ULL << len) - 1;
}

static uint64_t decode_wmask(int immn, int imms, int immr)
{
    uint64_t mask;
    int len = 31 - clz32((immn << 6) | (~imms & 0x3f));
    int esize = 1 << len;
    int levels = (esize - 1) & 0x3f;
    int s = imms & levels;
    int r = immr & levels;

    mask = bitmask(s + 1);
    mask = ((mask >> r) | (mask << (esize - r)));
    mask &= bitmask(esize);
    mask = replicate(mask, esize);

    return mask;
}

static void handle_orri(DisasContext *s, uint32_t insn)
{
    int is_32bit = !extract32(insn, 31, 1);
    int is_n = extract32(insn, 22, 1);
    int opc = extract32(insn, 29, 2);
    int dest = extract32(insn, 0, 5);
    int source = extract32(insn, 5, 5);
    int imms = extract32(insn, 10, 6);
    int immr = extract32(insn, 16, 6);
    TCGv_i64 tcg_dst;
    TCGv_i64 tcg_op2;
    bool setflags = false;
    uint64_t wmask;

    if (is_32bit && is_n) {
        /* reserved */
        unallocated_encoding(s);
    }

    if (opc == 0x3) {
        setflags = true;
    }

    if (setflags) {
        tcg_dst = cpu_reg(dest);
    } else {
        tcg_dst = cpu_reg_sp(dest);
    }

    wmask = decode_wmask(is_n, imms, immr);
    if (is_32bit) {
        wmask = (uint32_t)wmask;
    }
    tcg_op2 = tcg_const_i64(wmask);

    switch (opc) {
    case 0x3:
    case 0x0:
        tcg_gen_and_i64(tcg_dst, cpu_reg(source), tcg_op2);
        break;
    case 0x1:
        tcg_gen_or_i64(tcg_dst, cpu_reg(source), tcg_op2);
        break;
    case 0x2:
        tcg_gen_xor_i64(tcg_dst, cpu_reg(source), tcg_op2);
        break;
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_dst, tcg_dst);
    }

    if (setflags) {
        gen_helper_pstate_add(pstate, pstate, tcg_dst, cpu_reg(31), tcg_dst);
    }

    tcg_temp_free_i64(tcg_op2);
}

static void handle_extr(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int imms = extract32(insn, 10, 6);
    int rm = extract32(insn, 16, 5);
    bool is_32bit = !extract32(insn, 31, 1);
    int bitsize = is_32bit ? 32 : 64;
    TCGv_i64 tcg_res = tcg_temp_new_i64();
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_tmp, cpu_reg(rm));
    } else {
        tcg_gen_mov_i64(tcg_tmp, cpu_reg(rm));
    }
    tcg_gen_shri_i64(tcg_res, cpu_reg(rm), imms);
    tcg_gen_shli_i64(tcg_tmp, cpu_reg(rn), bitsize - imms);
    tcg_gen_or_i64(cpu_reg(rd), tcg_tmp, tcg_res);
    if (is_32bit) {
        tcg_gen_ext32u_i64(cpu_reg(rd), cpu_reg(rd));
    }

    tcg_temp_free_i64(tcg_tmp);
    tcg_temp_free_i64(tcg_res);
}

static void handle_bfm(DisasContext *s, uint32_t insn)
{
    bool is_32bit = !extract32(insn, 31, 1);
    int opc = extract32(insn, 29, 2);
    int dest = extract32(insn, 0, 5);
    int source = extract32(insn, 5, 5);
    int is_n = extract32(insn, 22, 1);
    int imms = extract32(insn, 10, 6);
    int immr = extract32(insn, 16, 6);
    TCGv_i64 tcg_newmask;
    uint64_t mask, tmask, topmask;
    uint64_t signbit = 1;
    int bitsize = is_32bit ? 32 : 64;

    if (!is_32bit && !is_n) {
        /* reserved */
        unallocated_encoding(s);
    }

    if (is_32bit && (is_n || (immr & (1 << 5)) || imms & (1 << 5))) {
        /* reserved */
        unallocated_encoding(s);
    }

    tcg_newmask = tcg_temp_new_i64();

    if (imms == 0x3f) {
        tmask = mask = ~0ULL;
    } else {
        tmask = mask = ((1ULL << (imms + 1)) - 1);
    }

    tcg_gen_andi_i64(tcg_newmask, cpu_reg(source), mask);
    if (imms < immr) {
        tcg_gen_shli_i64(tcg_newmask, tcg_newmask, bitsize - immr);
        tmask <<= bitsize - immr;
        signbit <<= bitsize + imms - immr;
        if (signbit == 0x8000000000000000ULL) {
            /* Can't pad anymore - highest bit is already set */
            topmask = 0;
        } else {
            topmask = ~((1ULL << (bitsize + imms - immr + 1)) - 1);
        }
    } else {
        tcg_gen_shri_i64(tcg_newmask, tcg_newmask, immr);
        tmask >>= immr;
        signbit <<= imms - immr;
        topmask = ~tmask;
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_newmask, tcg_newmask);
    }

    switch (opc) {
    case 0: { /* SBFM */
        TCGv_i64 tcg_mask = tcg_const_i64(topmask);
        TCGv_i64 tcg_signbit = tcg_const_i64(signbit);
        gen_helper_sign_extend(cpu_reg(dest), tcg_newmask, tcg_signbit,
                               tcg_mask);
        tcg_temp_free_i64(tcg_mask);
        tcg_temp_free_i64(tcg_signbit);
        break;
    }
    case 1: /* BFM */
        /* replace the field inside dest */
        tcg_gen_andi_i64(cpu_reg(dest), cpu_reg(dest), ~tmask);
        tcg_gen_or_i64(cpu_reg(dest), cpu_reg(dest), tcg_newmask);
        break;
    case 2: /* UBFM */
        tcg_gen_mov_i64(cpu_reg(dest), tcg_newmask);
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(cpu_reg(dest), cpu_reg(dest));
    }

    tcg_temp_free_i64(tcg_newmask);
}

static void ldst_calc_index(DisasContext *s, TCGv_i64 tcg_addr,
                            bool is_reg_offset, int offset, int size)
{
    bool is_shift = extract32(offset, 0, 1);
    int option = extract32(offset, 1, 3);
    int rn = extract32(offset, 4, 5);
    int shift = size;
    TCGv_i64 tcg_offset;

    if (!is_reg_offset) {
        tcg_offset = tcg_const_i64(offset);
        goto add_offset;
    }

    /* offset in register */
    if (!(option & 2)) {
        unallocated_encoding(s);
        return;
    }

    if (!is_shift) {
        shift = 0;
    }

    tcg_offset = tcg_temp_new_i64();
    reg_extend(tcg_offset, option, shift, rn);

add_offset:
    tcg_gen_add_i64(tcg_addr, tcg_addr, tcg_offset);
    tcg_temp_free_i64(tcg_offset);
}

static void handle_ldst(DisasContext *s, uint32_t insn)
{
    int dest = extract32(insn, 0, 5);
    int source = extract32(insn, 5, 5);
    bool wback = extract32(insn, 10, 1);
    int type = extract32(insn, 10, 2);
    bool is_reg_offset = extract32(insn, 21, 1);
    bool is_store = !extract32(insn, 22, 1);
    bool opc1 = extract32(insn, 23, 1);
    bool is_imm12 = extract32(insn, 24, 1);
    bool is_vector = extract32(insn, 26, 1);
    int size = extract32(insn, 30, 2);
    bool is_signed = false;
    bool postindex = false;
    TCGv_i64 tcg_addr;
    int offset;

    if (is_imm12) {
        /* wback, postindex and reg_offset bits are inside imm12 */
        postindex = false;
        wback = false;
        is_reg_offset = false;
    } else {
        /* These only apply to the IMM9 variant */
        if (is_reg_offset && type != 2) {
            unallocated_encoding(s);
            return;
        }
        /* LDR (post-index) */
        postindex = (type == 1);
    }

    if (is_vector) {
        size = (opc1 ? 0x4 : 0) | size;
        if (size > 4) {
            unallocated_encoding(s);
            return;
        }
    } else if (opc1) {
        if (size == 3) {
            /* prefetch */
            if (!is_store) {
                unallocated_encoding(s);
            }
            return;
        }
        if (size == 2 && !is_store) {
            unallocated_encoding(s);
        }
        is_store = false;
        is_signed = true;
    }

    if (is_imm12) {
        /* UIMM12 version */
        offset = extract32(insn, 10, 12) << size;
    } else {
        /* SIMM9 version */
        offset = sextract32(insn, 12, 9);
    }

    tcg_addr = tcg_temp_new_i64();

    tcg_gen_mov_i64(tcg_addr, cpu_reg_sp(source));

    if (!postindex) {
        ldst_calc_index(s, tcg_addr, is_reg_offset, offset, size);
    }

    ldst_do(s, dest, tcg_addr, size, is_store, is_signed, is_vector);

    if (postindex) {
        ldst_calc_index(s, tcg_addr, is_reg_offset, offset, size);
    }

    if (wback) {
        tcg_gen_mov_i64(cpu_reg_sp(source), tcg_addr);
    }

    tcg_temp_free_i64(tcg_addr);
}

static void handle_ld_literal(DisasContext *s, uint32_t insn)
{
    int dest = extract32(insn, 0, 5);
    int64_t imm = sextract32(insn, 5, 19) << 2;
    bool is_vector = extract32(insn, 26, 1);
    int opc = extract32(insn, 30, 2);
    TCGv_i64 tcg_addr;
    bool is_signed;
    int size;

    tcg_addr = tcg_const_i64((s->pc - 4) + imm);

    switch (opc) {
    case 0:
        is_signed = false;
        size = 2;
        break;
    case 1:
        is_signed = false;
        size = 3;
        break;
    case 2:
        is_signed = true;
        size = 2;
        break;
    case 3:
        /* prefetch */
        goto out;
    }

    ldst_do(s, dest, tcg_addr, size, false, is_signed, is_vector);

out:
    tcg_temp_free_i64(tcg_addr);
}

static void handle_cinc(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    TCGv_i32 tcg_insn = tcg_const_i32(insn);

    gen_helper_cinc(cpu_reg(rd), pstate, tcg_insn, cpu_reg(rn), cpu_reg(rm));
}

static void handle_div(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    bool is_signed = extract32(insn, 10, 1);
    bool is_32bit = !extract32(insn, 31, 1);
    TCGv_i64 n = tcg_temp_new_i64();
    TCGv_i64 m = tcg_temp_new_i64();

    if (is_32bit) {
        if (is_signed) {
            tcg_gen_ext32s_i64(n, cpu_reg(rn));
            tcg_gen_ext32s_i64(m, cpu_reg(rm));
        } else {
            tcg_gen_ext32u_i64(n, cpu_reg(rn));
            tcg_gen_ext32u_i64(m, cpu_reg(rm));
        }
    } else {
        tcg_gen_mov_i64(n, cpu_reg(rn));
        tcg_gen_mov_i64(m, cpu_reg(rm));
    }

    if (is_signed) {
        gen_helper_sdiv64(cpu_reg(rd), n, m);
    } else {
        gen_helper_udiv64(cpu_reg(rd), n, m);
    }

    if (is_32bit) {
        if (is_signed) {
            tcg_gen_ext32s_i64(cpu_reg(rd), cpu_reg(rd));
        } else {
            tcg_gen_ext32u_i64(cpu_reg(rd), cpu_reg(rd));
        }
    }

    tcg_temp_free_i64(n);
    tcg_temp_free_i64(m);
}

static void handle_shift_reg(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int shift_type  = extract32(insn, 10, 2);
    int rm = extract32(insn, 16, 5);
    bool is_32bit = !extract32(insn, 31, 1);
    TCGv_i64 tcg_shift;
    TCGv_i64 tcg_shifted;

    tcg_shift = tcg_temp_new_i64();
    tcg_gen_andi_i64(tcg_shift, cpu_reg(rm), is_32bit ? 31 : 63);
    tcg_shifted = get_shift(rn, shift_type, tcg_shift, is_32bit);
    tcg_gen_mov_i64(cpu_reg(rd), tcg_shifted);
    tcg_temp_free_i64(tcg_shift);
    tcg_temp_free_i64(tcg_shifted);
}

static void handle_rev(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opc = extract32(insn, 10, 2);
    bool is_32bit = !extract32(insn, 31, 1);
    TCGv_i32 tcg_tmp;

    switch (opc) {
    case 0x0: /* RBIT */
        if (is_32bit) {
            tcg_tmp = tcg_temp_new_i32();
            tcg_gen_trunc_i64_i32(tcg_tmp, cpu_reg(rn));
            gen_helper_rbit(tcg_tmp, tcg_tmp);
            tcg_gen_extu_i32_i64(cpu_reg(rd), tcg_tmp);
            tcg_temp_free_i32(tcg_tmp);
        } else {
            gen_helper_rbit64(cpu_reg(rd), cpu_reg(rn));
        }
        break;
    case 0x1: /* REV16 */
        tcg_gen_bswap16_i64(cpu_reg(rd), cpu_reg(rn));
        break;
    case 0x2: /* REV32 */
        tcg_gen_bswap32_i64(cpu_reg(rd), cpu_reg(rn));
        break;
    case 0x3: /* REV64 */
        tcg_gen_bswap64_i64(cpu_reg(rd), cpu_reg(rn));
        break;
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(cpu_reg(rd), cpu_reg(rd));
    }
}

static void handle_clz(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opc = extract32(insn, 10, 2);
    bool is_32bit = !extract32(insn, 31, 1);
    TCGv_i64 tcg_val = tcg_temp_new_i64();

    if (is_32bit) {
        tcg_gen_ext32u_i64(tcg_val, cpu_reg(rn));
    } else {
        tcg_gen_mov_i64(tcg_val, cpu_reg(rn));
    }

    switch (opc) {
    case 0x0: /* CLZ */
        gen_helper_clz64(cpu_reg(rd), tcg_val);
        if (is_32bit) {
            tcg_gen_subi_i64(cpu_reg(rd), cpu_reg(rd), 32);
        }
        break;
    case 0x1: /* CLS */
        unallocated_encoding(s);
        break;
    }

    tcg_temp_free_i64(tcg_val);
}

static void handle_mulh(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    bool is_signed = !extract32(insn, 23, 1);

    if (is_signed) {
        gen_helper_smulh(cpu_reg(rd), cpu_reg(rn), cpu_reg(rm));
    } else {
        gen_helper_umulh(cpu_reg(rd), cpu_reg(rn), cpu_reg(rm));
    }
}

/* Data-processing (3 source) */
static void handle_dp3s(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int ra = extract32(insn, 10, 5);
    int rm = extract32(insn, 16, 5);
    int op_id = (extract32(insn, 29, 3) << 4) |
                (extract32(insn, 21, 3) << 1) |
                extract32(insn, 15, 1);
    bool is_32bit = !(op_id & 0x40);
    bool is_sub = op_id & 0x1;
    bool is_signed = (op_id >= 0x42) && (op_id <= 0x44);
    bool is_high = op_id & 0x4;
    TCGv_i64 tcg_op1;
    TCGv_i64 tcg_op2;
    TCGv_i64 tcg_tmp;

    switch (op_id) {
    case 0x0: /* MADD (32bit) */
    case 0x1: /* MSUB (32bit) */
    case 0x40: /* MADD (64bit) */
    case 0x41: /* MSUB (64bit) */
    case 0x42: /* SMADDL */
    case 0x43: /* SMSUBL */
    case 0x44: /* SMULH */
    case 0x4a: /* UMADDL */
    case 0x4b: /* UMSUBL */
    case 0x4c: /* UMULH */
        break;
    default:
        unallocated_encoding(s);
    }

    if (is_high) {
        handle_mulh(s, insn);
        return;
    }

    tcg_op1 = tcg_temp_new_i64();
    tcg_op2 = tcg_temp_new_i64();
    tcg_tmp = tcg_temp_new_i64();

    if (op_id < 0x42) {
        tcg_gen_mov_i64(tcg_op1, cpu_reg(rn));
        tcg_gen_mov_i64(tcg_op2, cpu_reg(rm));
    } else {
        if (is_signed) {
            tcg_gen_ext32s_i64(tcg_op1, cpu_reg(rn));
            tcg_gen_ext32s_i64(tcg_op2, cpu_reg(rm));
        } else {
            tcg_gen_ext32u_i64(tcg_op1, cpu_reg(rn));
            tcg_gen_ext32u_i64(tcg_op2, cpu_reg(rm));
        }
    }

    tcg_gen_mul_i64(tcg_tmp, tcg_op1, tcg_op2);

    if (is_sub) {
        tcg_gen_sub_i64(cpu_reg(rd), cpu_reg(ra), tcg_tmp);
    } else {
        tcg_gen_add_i64(cpu_reg(rd), cpu_reg(ra), tcg_tmp);
    }

    if (is_32bit) {
        tcg_gen_ext32u_i64(cpu_reg(rd), cpu_reg(rd));
    }

    tcg_temp_free_i64(tcg_op1);
    tcg_temp_free_i64(tcg_op2);
    tcg_temp_free_i64(tcg_tmp);
}

static void handle_fpfpcvt(DisasContext *s, uint32_t insn, bool direction,
                           int rmode)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int scale = extract32(insn, 10, 6);
    int opcode = extract32(insn, 16, 3);
    int type = extract32(insn, 22, 2);
    bool is_32bit = !extract32(insn, 31, 1);
    bool is_double = extract32(type, 0, 1);
    bool is_signed = !extract32(opcode, 0, 1);
    int freg_offs;
    int fp_reg;
    TCGv_i64 tcg_int;
    TCGv_i32 tcg_single;
    TCGv_i64 tcg_double;
    TCGv_ptr tcg_fpstatus = get_fpstatus_ptr();
    TCGv_i32 tcg_shift = tcg_const_i32(scale);
    TCGv_i32 tcg_rmode = tcg_const_i32(rmode);
    TCGv_i64 tcg_tmp;

    if (direction) {
        fp_reg = rn;
        tcg_int = cpu_reg(rd);
    } else {
        fp_reg = rd;
        tcg_int = cpu_reg(rn);
    }
    freg_offs = offsetof(CPUARMState, vfp.regs[fp_reg * 2]);

    if (!direction) {
        clear_fpreg(fp_reg);
    }

    if (is_32bit && !direction) {
        tcg_tmp = tcg_temp_new_i64();
        if (is_signed) {
            tcg_gen_ext32s_i64(tcg_tmp, tcg_int);
        } else {
            tcg_gen_ext32u_i64(tcg_tmp, tcg_int);
        }
        tcg_int = tcg_tmp;
    }

    gen_helper_set_rmode(tcg_rmode, tcg_fpstatus);

    switch ((direction ? 0x10 : 0)|
            (is_double ? 0x1 : 0) |
            (is_signed ? 0x2 : 0)) {
    case 0x0: /* unsigned scalar->single */
        tcg_single = tcg_temp_new_i32();
        tcg_tmp = tcg_temp_new_i64();
        gen_helper_vfp_uqtos(tcg_single, tcg_int, tcg_shift, tcg_fpstatus);
        tcg_gen_extu_i32_i64(tcg_tmp, tcg_single);
        tcg_gen_st32_i64(tcg_tmp, cpu_env, freg_offs);
        tcg_temp_free_i32(tcg_single);
        tcg_temp_free_i64(tcg_tmp);
        break;
    case 0x1: /* unsigned scalar->double */
        tcg_double = tcg_temp_new_i64();
        gen_helper_vfp_uqtod(tcg_double, tcg_int, tcg_shift, tcg_fpstatus);
        tcg_gen_st_i64(tcg_double, cpu_env, freg_offs);
        tcg_temp_free_i64(tcg_double);
        break;
    case 0x2: /* signed scalar->single */
        tcg_single = tcg_temp_new_i32();
        tcg_tmp = tcg_temp_new_i64();
        gen_helper_vfp_sqtos(tcg_single, tcg_int, tcg_shift, tcg_fpstatus);
        tcg_gen_extu_i32_i64(tcg_tmp, tcg_single);
        tcg_gen_st32_i64(tcg_tmp, cpu_env, freg_offs);
        tcg_temp_free_i32(tcg_single);
        tcg_temp_free_i64(tcg_tmp);
        break;
    case 0x3: /* signed scalar->double */
        tcg_double = tcg_temp_new_i64();
        gen_helper_vfp_sqtod(tcg_double, tcg_int, tcg_shift, tcg_fpstatus);
        tcg_gen_st_i64(tcg_double, cpu_env, freg_offs);
        tcg_temp_free_i64(tcg_double);
        break;
    case 0x10: /* unsigned single->scalar */
        tcg_single = tcg_temp_new_i32();
        tcg_tmp = tcg_temp_new_i64();
        tcg_gen_ld32u_i64(tcg_tmp, cpu_env, freg_offs);
        tcg_gen_trunc_i64_i32(tcg_single, tcg_tmp);
        gen_helper_vfp_touqs(tcg_int, tcg_single, tcg_shift, tcg_fpstatus);
        tcg_temp_free_i32(tcg_single);
        tcg_temp_free_i64(tcg_tmp);
        break;
    case 0x11: /* unsigned single->double */
        tcg_double = tcg_temp_new_i64();
        tcg_gen_ld_i64(tcg_double, cpu_env, freg_offs);
        gen_helper_vfp_touqd(tcg_int, tcg_double, tcg_shift, tcg_fpstatus);
        tcg_temp_free_i64(tcg_double);
        break;
    case 0x12: /* signed single->scalar */
        tcg_single = tcg_temp_new_i32();
        tcg_tmp = tcg_temp_new_i64();
        tcg_gen_ld32u_i64(tcg_tmp, cpu_env, freg_offs);
        tcg_gen_trunc_i64_i32(tcg_single, tcg_tmp);
        gen_helper_vfp_tosqs(tcg_int, tcg_single, tcg_shift, tcg_fpstatus);
        tcg_temp_free_i32(tcg_single);
        tcg_temp_free_i64(tcg_tmp);
        break;
    case 0x13: /* signed single->double */
        tcg_double = tcg_temp_new_i64();
        tcg_gen_ld_i64(tcg_double, cpu_env, freg_offs);
        gen_helper_vfp_tosqd(tcg_int, tcg_double, tcg_shift, tcg_fpstatus);
        tcg_temp_free_i64(tcg_double);
        break;
    default:
        unallocated_encoding(s);
    }

    /* XXX use fpcr */
    tcg_gen_movi_i32(tcg_rmode, -1);
    gen_helper_set_rmode(tcg_rmode, tcg_fpstatus);

    if (is_32bit && direction) {
        tcg_gen_ext32u_i64(tcg_int, tcg_int);
    }

    tcg_temp_free_ptr(tcg_fpstatus);
    tcg_temp_free_i32(tcg_shift);
    tcg_temp_free_i32(tcg_rmode);
}

/* fixed <-> floating conversion */
static void handle_fpfpconv(DisasContext *s, uint32_t insn)
{
    int opcode = extract32(insn, 16, 3);
    int rmode = extract32(insn, 20, 2);
    int type = extract32(insn, 22, 2);
    bool is_s = extract32(insn, 29, 1);
    bool direction;

    if (is_s || (type > 1) || (opcode > 1)) {
        unallocated_encoding(s);
        return;
    }

    switch (rmode) {
    case 0x1: /* [S|U]CVTF (scalar->float) */
        direction = 0;
        break;
    case 0x3: /* FCVTZ[S|U] (float->scalar) */
        direction = 1;
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    handle_fpfpcvt(s, insn, direction, ROUND_MODE_ZERO);
}

/* fmov (immediate) */
static void handle_fmovi(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int imm8 = extract32(insn, 13, 8);
    int is_double = extract32(insn, 22, 2);
    uint64_t imm;
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    TCGv_i64 tcg_res;

    if (is_double > 1) {
        unallocated_encoding(s);
        return;
    }

    if (is_double) {
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
              (extract32(imm8, 6, 1) ? 0x3fc0 : 0x4000) |
              extract32(imm8, 0, 6);
        imm <<= 48;
    } else {
        imm = (extract32(imm8, 7, 1) ? 0x8000 : 0) |
              (extract32(imm8, 6, 1) ? 0x3e00 : 0x4000) |
              (extract32(imm8, 0, 6) << 3);
        imm <<= 16;
    }

    tcg_res = tcg_const_i64(imm);
    clear_fpreg(rd);
    tcg_gen_st_i64(tcg_res, cpu_env, freg_offs_d);
    tcg_temp_free_i64(tcg_res);
}

/* floating <-> integer conversion */
static void handle_fpintconv(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 16, 3);
    int rmode = extract32(insn, 19, 2);
    int type = extract32(insn, 22, 2);
    bool is_s = extract32(insn, 29, 1);
    bool is_32bit = !extract32(insn, 31, 1);

    if (!is_s && (rmode < 2) && (opcode > 5)) {
        /* FMOV */
        bool itof = opcode & 1;
        int dest = itof ? rd : rn;
        int freg_offs = offsetof(CPUARMState, vfp.regs[dest * 2]);

        if (rmode & 1) {
            freg_offs += sizeof(float64);
        }

        if (itof && (!(rmode & 1))) {
            clear_fpreg(dest);
        }

        switch (type |
                ((rmode & 1) ? 0x4 : 0) |
                (itof ? 0x8 : 0)) {
        case 0x0:
            tcg_gen_ld32u_i64(cpu_reg(rd), cpu_env, freg_offs);
            break;
        case 0x1:
        case 0x2 | 0x4:
            tcg_gen_ld_i64(cpu_reg(rd), cpu_env, freg_offs);
            break;
        case 0x8 | 0x0:
            tcg_gen_st32_i64(cpu_reg(rn), cpu_env, freg_offs);
            break;
        case 0x8 | 0x1:
        case 0x8 | 0x2 | 0x4:
            tcg_gen_st_i64(cpu_reg(rn), cpu_env, freg_offs);
            break;
        default:
            unallocated_encoding(s);
        }

        if (is_32bit && !itof) {
            tcg_gen_ext32u_i64(cpu_reg(rd), cpu_reg(rd));
        }
    } else if (!is_s && ((opcode & 0x6) < 5)) {
        /* [S|U]CVTF and FCVT[N|P|M|Z][S|U] */
        handle_fpfpcvt(s, insn, !(opcode & 0x6), rmode);
    } else {
        /* XXX */
        unallocated_encoding(s);
    }
}

/* Floating-point compare */
static void handle_fcmp(DisasContext *s, uint32_t insn)
{
    int opc = extract32(insn, 3, 2);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    bool is_32bit = !extract32(insn, 22, 1);
    bool is_cmp_with_zero = extract32(opc, 0, 1);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int freg_offs_m = offsetof(CPUARMState, vfp.regs[rm * 2]);
    TCGv_i32 tcg_op1_32 = tcg_temp_new_i32();
    TCGv_i64 tcg_op1_64 = tcg_temp_new_i64();
    TCGv_i32 tcg_op2_32;
    TCGv_i64 tcg_op2_64;

    if (extract32(insn, 23, 1)) {
        unallocated_encoding(s);
    }

    tcg_gen_ld_i64(tcg_op1_64, cpu_env, freg_offs_n);

    if (is_32bit) {
        tcg_gen_trunc_i64_i32(tcg_op1_32, tcg_op1_64);
    }

    if (is_cmp_with_zero) {
        tcg_op2_32 = tcg_const_i32(0);
        tcg_op2_64 = tcg_const_i64(0);
    } else {
        tcg_op2_32 = tcg_temp_new_i32();
        tcg_op2_64 = tcg_temp_new_i64();

        tcg_gen_ld_i64(tcg_op2_64, cpu_env, freg_offs_m);
        if (is_32bit) {
            tcg_gen_trunc_i64_i32(tcg_op2_32, tcg_op2_64);
        }
    }

    switch (opc | (is_32bit ? 0x0 : 0x4)) {
    case 0x0: /* FCMP single, 32bit */
    case 0x1: /* FCMPZ single, 32bit */
        gen_helper_vfp_cmps(pstate, tcg_op1_32, tcg_op2_32, cpu_env);
        break;
    case 0x2: /* FCMPE single, 32bit */
    case 0x3: /* FCMPEZ single, 32bit */
        gen_helper_vfp_cmpes(pstate, tcg_op1_32, tcg_op2_32, cpu_env);
        break;
    case 0x4: /* FCMP double, 64bit */
    case 0x5: /* FCMPZ double, 64bit */
        gen_helper_vfp_cmpd(pstate, tcg_op1_64, tcg_op2_64, cpu_env);
        break;
    case 0x6: /* FCMPE double, 64bit */
    case 0x7: /* FCMPEZ double, 64bit */
        gen_helper_vfp_cmped(pstate, tcg_op1_64, tcg_op2_64, cpu_env);
        break;
    }

    tcg_temp_free_i64(tcg_op1_64);
    tcg_temp_free_i64(tcg_op2_64);
    tcg_temp_free_i32(tcg_op1_32);
    tcg_temp_free_i32(tcg_op2_32);
}

/* Floating-point data-processing (1 source) - 32 bit */
static void handle_fpdp1s32(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 15, 6);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    TCGv_i32 tcg_op = tcg_temp_new_i32();
    TCGv_i32 tcg_res = tcg_temp_new_i32();
    TCGv_ptr fpst = get_fpstatus_ptr();
    bool skip_write = false;

    tcg_gen_ld_i64(tcg_tmp, cpu_env, freg_offs_n);
    tcg_gen_trunc_i64_i32(tcg_op, tcg_tmp);

    switch (opcode) {
    case 0x0: /* FMOV */
        tcg_gen_mov_i32(tcg_res, tcg_op);
        break;
    case 0x1: /* FABS */
        gen_helper_vfp_abss(tcg_res, tcg_op);
        break;
    case 0x2: /* FNEG */
        gen_helper_vfp_negs(tcg_res, tcg_op);
        break;
    case 0x3: /* FSQRT */
        gen_helper_vfp_sqrts(tcg_res, tcg_op, cpu_env);
        break;
    case 0x5: /* FCVT (single to double) */
        skip_write = true;
        gen_helper_vfp_fcvtds(tcg_tmp, tcg_op, cpu_env);
        clear_fpreg(rd);
        tcg_gen_st_i64(tcg_tmp, cpu_env, freg_offs_d);
        break;
    case 0x7: /* FCVT (single to half) */
        /* XXX */
        unallocated_encoding(s);
        return;
    case 0x8: /* FRINTN XXX add rounding mode */
    case 0x9: /* FRINTP */
    case 0xa: /* FRINTM */
    case 0xb: /* FRINTZ */
    case 0xc: /* FRINTA */
    case 0xe: /* FRINTX */
    case 0xf: /* FRINTI */
    {
        TCGv_i32 tcg_rmode = tcg_const_i32(opcode & 7);

        gen_helper_set_rmode(tcg_rmode, fpst);
        gen_helper_rints(tcg_res, tcg_op, fpst);

        /* XXX use fpcr */
        tcg_gen_movi_i32(tcg_rmode, -1);
        gen_helper_set_rmode(tcg_rmode, fpst);
        tcg_temp_free_i32(tcg_rmode);
        break;
    }
    default:
        unallocated_encoding(s);
        return;
    }

    if (!skip_write) {
        clear_fpreg(rd);
        tcg_gen_extu_i32_i64(tcg_tmp, tcg_res);
        tcg_gen_st32_i64(tcg_tmp, cpu_env, freg_offs_d);
    }

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i32(tcg_op);
    tcg_temp_free_i32(tcg_res);
    tcg_temp_free_i64(tcg_tmp);
}

/* Floating-point data-processing (1 source) - 64 bit */
static void handle_fpdp1s64(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 15, 6);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    TCGv_i64 tcg_op = tcg_temp_new_i64();
    TCGv_i64 tcg_res = tcg_temp_new_i64();
    TCGv_i32 tcg_tmp32;
    TCGv_ptr fpst = get_fpstatus_ptr();

    tcg_gen_ld_i64(tcg_op, cpu_env, freg_offs_n);

    switch (opcode) {
    case 0x0: /* FMOV */
        tcg_gen_mov_i64(tcg_res, tcg_op);
        break;
    case 0x1: /* FABS */
        gen_helper_vfp_absd(tcg_res, tcg_op);
        break;
    case 0x2: /* FNEG */
        gen_helper_vfp_negd(tcg_res, tcg_op);
        break;
    case 0x3: /* FSQRT */
        gen_helper_vfp_sqrtd(tcg_res, tcg_op, cpu_env);
        break;
    case 0x4: /* FCVT (double to single) */
        tcg_tmp32 = tcg_temp_new_i32();
        gen_helper_vfp_fcvtsd(tcg_tmp32, tcg_op, cpu_env);
        tcg_gen_extu_i32_i64(tcg_res, tcg_tmp32);
        tcg_temp_free_i32(tcg_tmp32);
        break;
    case 0x7: /* FCVT (double to half) */
        /* XXX */
        unallocated_encoding(s);
        return;
    case 0x8: /* FRINTN XXX add rounding mode */
    case 0x9: /* FRINTP */
    case 0xa: /* FRINTM */
    case 0xb: /* FRINTZ */
    case 0xc: /* FRINTA */
    case 0xe: /* FRINTX */
    case 0xf: /* FRINTI */
    {
        TCGv_i32 tcg_rmode = tcg_const_i32(opcode & 7);

        gen_helper_set_rmode(tcg_rmode, fpst);
        gen_helper_rintd(tcg_res, tcg_op, fpst);

        /* XXX use fpcr */
        tcg_gen_movi_i32(tcg_rmode, -1);
        gen_helper_set_rmode(tcg_rmode, fpst);
        tcg_temp_free_i32(tcg_rmode);
        break;
    }
    default:
        unallocated_encoding(s);
        return;
    }

    clear_fpreg(rd);
    tcg_gen_st_i64(tcg_res, cpu_env, freg_offs_d);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i64(tcg_op);
    tcg_temp_free_i64(tcg_res);
}

/* Floating-point data-processing (2 source) - 32 bit */
static void handle_fpdp2s32(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 12, 4);
    int rm = extract32(insn, 16, 5);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int freg_offs_m = offsetof(CPUARMState, vfp.regs[rm * 2]);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    TCGv_i32 tcg_op1 = tcg_temp_new_i32();
    TCGv_i32 tcg_op2 = tcg_temp_new_i32();
    TCGv_i32 tcg_res = tcg_temp_new_i32();
    TCGv_ptr fpst = get_fpstatus_ptr();

    tcg_gen_ld_i64(tcg_tmp, cpu_env, freg_offs_n);
    tcg_gen_trunc_i64_i32(tcg_op1, tcg_tmp);
    tcg_gen_ld_i64(tcg_tmp, cpu_env, freg_offs_m);
    tcg_gen_trunc_i64_i32(tcg_op2, tcg_tmp);

    switch (opcode) {
    case 0x0: /* FMUL */
        gen_helper_vfp_muls(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x8: /* FNMUL */
        gen_helper_vfp_muls(tcg_res, tcg_op1, tcg_op2, fpst);
        gen_helper_vfp_negs(tcg_res, tcg_res);
        break;
    case 0x1: /* FDIV */
        gen_helper_vfp_divs(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x2: /* FADD */
        gen_helper_vfp_adds(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    case 0x3: /* FSUB */
        gen_helper_vfp_subs(tcg_res, tcg_op1, tcg_op2, fpst);
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    clear_fpreg(rd);
    tcg_gen_extu_i32_i64(tcg_tmp, tcg_res);
    tcg_gen_st32_i64(tcg_tmp, cpu_env, freg_offs_d);

    tcg_temp_free_ptr(fpst);
    tcg_temp_free_i32(tcg_op1);
    tcg_temp_free_i32(tcg_op2);
    tcg_temp_free_i32(tcg_res);
    tcg_temp_free_i64(tcg_tmp);
}

/* SIMD ORR */
static void handle_simdorr(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int rm = extract32(insn, 16, 5);
    int size = extract32(insn, 22, 2);
    int opcode = extract32(insn, 11, 5);
    bool is_q = extract32(insn, 30, 1);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int freg_offs_m = offsetof(CPUARMState, vfp.regs[rm * 2]);
    TCGv_i64 tcg_op1_1 = tcg_temp_new_i64();
    TCGv_i64 tcg_op1_2 = tcg_temp_new_i64();
    TCGv_i64 tcg_op2_1 = tcg_temp_new_i64();
    TCGv_i64 tcg_op2_2 = tcg_temp_new_i64();
    TCGv_i64 tcg_res_1 = tcg_temp_new_i64();
    TCGv_i64 tcg_res_2 = tcg_temp_new_i64();

    tcg_gen_ld_i64(tcg_op1_1, cpu_env, freg_offs_n);
    tcg_gen_ld_i64(tcg_op2_1, cpu_env, freg_offs_m);
    if (is_q) {
        tcg_gen_ld_i64(tcg_op1_2, cpu_env, freg_offs_n + sizeof(float64));
        tcg_gen_ld_i64(tcg_op2_2, cpu_env, freg_offs_m + sizeof(float64));
    } else {
        tcg_gen_movi_i64(tcg_op1_2, 0);
        tcg_gen_movi_i64(tcg_op2_2, 0);
    }

    switch (opcode) {
    case 0x3: /* ORR */
        if (size & 1) {
            tcg_gen_not_i64(tcg_op2_1, tcg_op2_1);
            tcg_gen_not_i64(tcg_op2_2, tcg_op2_2);
        }
        if (size & 2) {
            tcg_gen_or_i64(tcg_res_1, tcg_op1_1, tcg_op2_1);
            tcg_gen_or_i64(tcg_res_2, tcg_op1_2, tcg_op2_2);
        } else {
            tcg_gen_and_i64(tcg_res_1, tcg_op1_1, tcg_op2_1);
            tcg_gen_and_i64(tcg_res_2, tcg_op1_2, tcg_op2_2);
        }
        break;
    default:
        unallocated_encoding(s);
        return;
    }

    tcg_gen_st_i64(tcg_res_1, cpu_env, freg_offs_d);
    if (!is_q) {
        tcg_gen_movi_i64(tcg_res_2, 0);
    }
    tcg_gen_st_i64(tcg_res_2, cpu_env, freg_offs_d + sizeof(float64));

    tcg_temp_free_i64(tcg_op1_1);
    tcg_temp_free_i64(tcg_op1_2);
    tcg_temp_free_i64(tcg_op2_1);
    tcg_temp_free_i64(tcg_op2_2);
    tcg_temp_free_i64(tcg_res_1);
    tcg_temp_free_i64(tcg_res_2);
}

/* AdvSIMD scalar three same (U=0) */
static void handle_simd3su0(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int opcode = extract32(insn, 11, 5);
    int rm = extract32(insn, 16, 5);
    int size = extract32(insn, 22, 2);
    bool is_sub = extract32(insn, 29, 1);
    bool is_q = extract32(insn, 30, 1);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    int freg_offs_m = offsetof(CPUARMState, vfp.regs[rm * 2]);
    TCGv_i64 tcg_op1 = tcg_temp_new_i64();
    TCGv_i64 tcg_op2 = tcg_temp_new_i64();
    TCGv_i64 tcg_res = tcg_temp_new_i64();
    int ebytes = (1 << size);
    int i;

    for (i = 0; i < 16; i += ebytes) {
        simd_ld(tcg_op1, freg_offs_n + i, size);
        simd_ld(tcg_op2, freg_offs_m + i, size);

        switch (opcode) {
        case 0x10: /* ADD / SUB */
            if (is_sub) {
                tcg_gen_sub_i64(tcg_res, tcg_op1, tcg_op2);
            } else {
                tcg_gen_add_i64(tcg_res, tcg_op1, tcg_op2);
            }
            break;
        default:
            unallocated_encoding(s);
            return;
        }

        simd_st(tcg_res, freg_offs_d + i, size);
    }

    if (!is_q) {
        TCGv_i64 tcg_zero = tcg_const_i64(0);
        simd_st(tcg_zero, freg_offs_d + sizeof(float64), 3);
        tcg_temp_free_i64(tcg_zero);
    }

    tcg_temp_free_i64(tcg_op1);
    tcg_temp_free_i64(tcg_op2);
    tcg_temp_free_i64(tcg_res);
}

/* AdvSIMD modified immediate */
static void handle_simdmodi(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int cmode = extract32(insn, 12, 4);
    uint64_t abcdefgh = extract32(insn, 5, 5) | (extract32(insn, 16, 3) << 5);
    bool is_neg = extract32(insn, 29, 1);
    bool is_q = extract32(insn, 30, 1);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    uint64_t imm = 0;
    int shift, i;
    TCGv_i64 tcg_op1_1 = tcg_temp_new_i64();
    TCGv_i64 tcg_op1_2 = tcg_temp_new_i64();
    TCGv_i64 tcg_res_1 = tcg_temp_new_i64();
    TCGv_i64 tcg_res_2 = tcg_temp_new_i64();
    TCGv_i64 tcg_imm;

    switch ((cmode >> 1) & 0x7) {
    case 0:
    case 1:
    case 2:
    case 3:
        shift = ((cmode >> 1) & 0x7) * 8;
        imm = (abcdefgh << shift) | (abcdefgh << (32 + shift));
        break;
    case 4:
    case 5:
        shift = ((cmode >> 1) & 0x1) * 8;
        imm = (abcdefgh << shift) |
              (abcdefgh << (16 + shift)) |
              (abcdefgh << (32 + shift)) |
              (abcdefgh << (48 + shift));
        break;
    case 6:
        if (cmode & 1) {
            imm = (abcdefgh << 8) |
                  (abcdefgh << 48) |
                  0x000000ff000000ffULL;
        } else {
            imm = (abcdefgh << 16) |
                  (abcdefgh << 56) |
                  0x0000ffff0000ffffULL;
        }
        break;
    case 7:
        if (!(cmode & 1) && !is_neg) {
            imm = abcdefgh |
                  (abcdefgh << 8) |
                  (abcdefgh << 16) |
                  (abcdefgh << 24) |
                  (abcdefgh << 32) |
                  (abcdefgh << 40) |
                  (abcdefgh << 48) |
                  (abcdefgh << 56);
        } else if (!(cmode & 1) && is_neg) {
            imm = 0;
            for (i = 0; i < 8; i++) {
                if ((abcdefgh) & (1 << (7 - i))) {
                    imm |= 0xffULL << (i * 8);
                }
            }
        } else if (cmode & 1) {
            shift = is_neg ? 48 : 19;
            imm = (abcdefgh & 0x1f) << 19;
            if (abcdefgh & 0x80) {
                imm |= 0x80000000;
            }
            if (!(abcdefgh & 0x40)) {
                imm |= 0x40000000;
            }
            if (abcdefgh & 0x20) {
                imm |= is_neg ? 0x3fc00000 : 0x3e000000;
            }
            imm |= (imm << 32);
        }
        shift = ((cmode >> 1) & 0x1) * 8;
        break;
    }

    if (is_neg) {
        imm = ~imm;
    }
    tcg_imm = tcg_const_i64(imm);

    tcg_gen_ld_i64(tcg_op1_1, cpu_env, freg_offs_d);
    if (is_q) {
        tcg_gen_ld_i64(tcg_op1_2, cpu_env, freg_offs_d + sizeof(float64));
    } else {
        tcg_gen_movi_i64(tcg_op1_2, 0);
    }

    if ((cmode == 0xf) && is_neg && !is_q) {
        unallocated_encoding(s);
        return;
    } else if ((cmode & 1) && is_neg) {
        /* AND (BIC) */
        tcg_gen_and_i64(tcg_res_1, tcg_op1_1, tcg_imm);
        tcg_gen_and_i64(tcg_res_2, tcg_op1_2, tcg_imm);
    } else if ((cmode & 1) && !is_neg) {
        /* ORR */
        tcg_gen_or_i64(tcg_res_1, tcg_op1_1, tcg_imm);
        tcg_gen_or_i64(tcg_res_2, tcg_op1_2, tcg_imm);
    } else {
        /* MOVI */
        tcg_gen_mov_i64(tcg_res_1, tcg_imm);
        tcg_gen_mov_i64(tcg_res_2, tcg_imm);
    }

    tcg_gen_st_i64(tcg_res_1, cpu_env, freg_offs_d);
    if (!is_q) {
        tcg_gen_movi_i64(tcg_res_2, 0);
    }
    tcg_gen_st_i64(tcg_res_2, cpu_env, freg_offs_d + sizeof(float64));

    tcg_temp_free_i64(tcg_op1_1);
    tcg_temp_free_i64(tcg_op1_2);
    tcg_temp_free_i64(tcg_res_1);
    tcg_temp_free_i64(tcg_res_2);
    tcg_temp_free_i64(tcg_imm);
}

/* SIMD shift ushll */
static void handle_ushll(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int immh = extract32(insn, 19, 4);
    bool is_q = extract32(insn, 30, 1);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    int i;
    int ebytes;
    int size;
    int shift = extract32(insn, 16, 7);

    if (is_q) {
        freg_offs_n += sizeof(float64);
    }

    for (size = 0; !(immh & (1 << size)); size++) {
        if (size > 3) {
            unallocated_encoding(s);
            return;
        }
    }

    ebytes = 1 << size;
    shift -= (8 << size);

    if (!immh) {
        /* XXX see asimdimm */
        unallocated_encoding(s);
        return;
    }

    if (immh & 0x8) {
        unallocated_encoding(s);
        return;
    }

    for (i = 0; i < (8 / ebytes); i++) {
        simd_ld(tcg_tmp, freg_offs_n + (i * ebytes), size);
        tcg_gen_shli_i64(tcg_tmp, tcg_tmp, shift);
        simd_st(tcg_tmp, freg_offs_d + (i * ebytes * 2), size + 1);
    }

    tcg_temp_free_i64(tcg_tmp);
}

/* SIMD shift shl */
static void handle_simdshl(DisasContext *s, uint32_t insn)
{
    int rd = extract32(insn, 0, 5);
    int rn = extract32(insn, 5, 5);
    int immh = extract32(insn, 19, 4);
    bool is_q = extract32(insn, 30, 1);
    int freg_offs_d = offsetof(CPUARMState, vfp.regs[rd * 2]);
    int freg_offs_n = offsetof(CPUARMState, vfp.regs[rn * 2]);
    TCGv_i64 tcg_tmp = tcg_temp_new_i64();
    int i;
    int ebytes;
    int size;
    int shift = extract32(insn, 16, 7);

    size = clz32(immh) - (31 - 4);

    if (size > 3) {
        /* XXX implement 128bit operations */
        unallocated_encoding(s);
        return;
    }

    ebytes = 1 << size;
    shift -= (8 << size);

    if (!immh) {
        /* XXX see asimdimm */
        unallocated_encoding(s);
        return;
    }

    for (i = 0; i < (16 / ebytes); i++) {
        simd_ld(tcg_tmp, freg_offs_n + (i * ebytes), size);
        tcg_gen_shli_i64(tcg_tmp, tcg_tmp, shift);
        simd_st(tcg_tmp, freg_offs_d + (i * ebytes), size);
    }

    if (!is_q) {
        TCGv_i64 tcg_zero = tcg_const_i64(0);
        simd_st(tcg_zero, freg_offs_d + sizeof(float64), 3);
        tcg_temp_free_i64(tcg_zero);
    }

    tcg_temp_free_i64(tcg_tmp);
}

static void handle_svc(DisasContext *s, uint32_t insn)
{
    gen_a64_set_pc_im(s->pc);
    s->is_jmp = DISAS_SWI;
}

static void handle_mrs(DisasContext *s, uint32_t insn)
{
    int dest = extract32(insn, 0, 5);
    int op2 = extract32(insn, 5, 3);
    int crm = extract32(insn, 8, 4);
    int crn = extract32(insn, 12, 4);
    int op1 = extract32(insn, 16, 3);
    int op0 = extract32(insn, 19, 2);

    /* XXX handle properly */
    if (op0 == 3 && op1 == 3 && op2 == 2 && !crm && crn == 13) {
        tcg_gen_ld_i64(cpu_reg(dest), cpu_env,
                       offsetof(CPUARMState, sr.tpidr_el0));
    } else if (op0 == 3 && op1 == 3 && (op2 == 0 || op2 == 1) &&
               crm == 4 && crn == 4) {
        /* XXX this is probably wrong! */
        tcg_gen_ld32u_i64(cpu_reg(dest), cpu_env,
            offsetof(CPUARMState, vfp.xregs[ARM_VFP_FPSCR]));
    } else if (op0 == 3 && op1 == 3 && op2 == 1 && crm == 0 && crn == 0) {
        /*
         * CTR_EL0 [3:0] contains log2 of icache line size in words.
         * CTR_EL0 [19:16] contains log2 of dcache line size in words.
         */
        tcg_gen_movi_i64(cpu_reg(dest), 0x30003);
    } else {
        qemu_log_mask(LOG_UNIMP, "MRS: %d %d %d %d %d\n", op0, op1, op2, crm,
                      crn);
        unallocated_encoding(s);
    }
}

static void handle_msr(DisasContext *s, uint32_t insn)
{
    int dest = extract32(insn, 0, 5);
    int op2 = extract32(insn, 5, 3);
    int crm = extract32(insn, 8, 4);
    int crn = extract32(insn, 12, 4);
    int op1 = extract32(insn, 16, 3);
    int op0 = extract32(insn, 19, 2);

    /* XXX handle properly */
    if (op0 == 3 && op1 == 3 && op2 == 2 && !crm && crn == 13) {
        tcg_gen_st_i64(cpu_reg(dest), cpu_env,
                       offsetof(CPUARMState, sr.tpidr_el0));
    } else if (op0 == 3 && op1 == 3 && (op2 == 0 || op2 == 1) &&
               crm == 4 && crn == 4) {
        /* XXX this is probably wrong! */
        tcg_gen_st32_i64(cpu_reg(dest), cpu_env,
            offsetof(CPUARMState, vfp.xregs[ARM_VFP_FPSCR]));
    } else {
        qemu_log_mask(LOG_UNIMP, "MSR: %d %d %d %d %d\n", op0, op1, op2, crm,
                      crn);
        unallocated_encoding(s);
    }
}

static void handle_sys(DisasContext *s, uint32_t insn)
{
    /*
     * XXX Simply ignore sys for now. We only need to start worrying about it
     *     when we start implementing system emulation.
     */
}

void disas_a64_insn(CPUARMState *env, DisasContext *s)
{
    uint32_t insn;

    insn = arm_ldl_code(env, s->pc, s->bswap_code);
    s->insn = insn;
    s->pc += 4;

    /* One-off branch instruction layout */
    switch (insn >> 26) {
    case 0x25:
    case 0x5:
        handle_b(s, insn);
        goto insn_done;
    case 0x35:
        if ((insn & 0xff9ffc1f) == 0xd61f0000) {
            handle_br(s, insn);
            goto insn_done;
        }
        break;
    }

    /* Typical major opcode encoding */
    switch ((insn >> 24) & 0x1f) {
    case 0x08:
    case 0x09:
        if (extract32(insn, 29, 1)) {
            handle_stp(s, insn);
        } else {
            handle_ldarx(s, insn);
        }
        break;
    case 0x0a:
        handle_orr(s, insn);
        break;
    case 0x0b:
        handle_add(s, insn);
        break;
    case 0x0c:
        if (extract32(insn, 29, 1)) {
            handle_stp(s, insn);
        } else if (!extract32(insn, 31, 1) && extract32(insn, 23, 1) &&
                   !extract32(insn, 21, 1)) {
            handle_simdldstm(s, insn, true);
        } else if (!extract32(insn, 31, 1) && !extract32(insn, 29, 1) &&
                   !extract32(insn, 23, 1) && !extract32(insn, 16, 6)) {
            handle_simdldstm(s, insn, false);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x0d:
        if (extract32(insn, 29, 1)) {
            handle_stp(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x0e:
        if (!extract32(insn, 31, 1) && !extract32(insn, 29, 1) &&
            (extract32(insn, 10, 6) == 0x3)) {
            handle_dupg(s, insn);
        } else if (!extract32(insn, 31, 1) && !extract32(insn, 29, 1) &&
            (extract32(insn, 10, 6) == 0xf)) {
            handle_umov(s, insn);
        } else if ((extract32(insn, 29, 3) == 2) && !extract32(insn, 21, 3) &&
            (extract32(insn, 10, 6) == 0x7)) {
            handle_insg(s, insn);
        } else if (!extract32(insn, 31, 1) && !extract32(insn, 29, 1) &&
                   extract32(insn, 21, 1) && extract32(insn, 10, 1) &&
                   (extract32(insn, 11, 5) == 0x3)) {
            handle_simdorr(s, insn);
        } else if (!extract32(insn, 31, 1) && extract32(insn, 21, 1) &&
                   extract32(insn, 10, 1)) {
            handle_simd3su0(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x0f:
        if (!extract32(insn, 31, 1) && !extract32(insn, 19, 5) &&
            (extract32(insn, 10, 2) == 1)) {
            handle_simdmodi(s, insn);
        } else if (!extract32(insn, 31, 1) && !extract32(insn, 23, 1) &&
                   (extract32(insn, 10, 6) == 0x29)) {
            handle_ushll(s, insn);
        } else if (!extract32(insn, 31, 1) && !extract32(insn, 23, 1) &&
                   (extract32(insn, 10, 6) == 0x15)) {
            handle_simdshl(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x10:
        handle_adr(s, insn);
        break;
    case 0x11:
        handle_addi(s, insn);
        break;
    case 0x12:
        if (extract32(insn, 23, 1)) {
            handle_movi(s, insn);
        } else {
            handle_orri(s, insn);
        }
        break;
    case 0x13:
        if (extract32(insn, 23, 1) && !extract32(insn, 21, 1) &&
            !extract32(insn, 29, 2)) {
            handle_extr(s, insn);
        } else {
            handle_bfm(s, insn);
        }
        break;
    case 0x14:
        if (extract32(insn, 29, 3) == 0x6 && !extract32(insn, 2, 3)) {
            handle_svc(s, insn);
        } else if (extract32(insn, 29, 2) == 0x1) {
            handle_cb(s, insn);
        } else if (extract32(insn, 29, 3) == 0x2) {
            handle_condb(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x15:
        if (extract32(insn, 29, 2) == 0x1) {
            handle_cb(s, insn);
        } else if (extract32(insn, 20, 12) == 0xd53) {
            handle_mrs(s, insn);
        } else if (extract32(insn, 20, 12) == 0xd51) {
            handle_msr(s, insn);
        } else if ((insn & 0xfffff01f) == 0xd503201f) {
            /* HINT instructions, do nothing */
        } else if ((insn & 0xfffff09f) == 0xd503309f) {
            /* barrier instructions, do nothing */
        } else if (extract32(insn, 19, 13) == 0x1aa1) {
            handle_sys(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x16:
    case 0x17:
        if (extract32(insn, 29, 2) == 0x1) {
            handle_tbz(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x18:
    case 0x19:
    case 0x1c:
    case 0x1d:
        if (extract32(insn, 29, 1)) {
            handle_ldst(s, insn);
        } else {
            handle_ld_literal(s, insn);
        }
        break;
    case 0x1a:
        if ((insn & 0x3fe00800) == 0x1a800000) {
            handle_cinc(s, insn);
        } else if ((insn & 0x7fe0f800) == 0x1ac00800) {
            handle_div(s, insn);
        } else if ((insn & 0x7fe0f000) == 0x1ac02000) {
            handle_shift_reg(s, insn);
        } else if ((insn & 0x7ffff000) == 0x5ac00000) {
            handle_rev(s, insn);
        } else if ((insn & 0x7ffff800) == 0x5ac01000) {
            handle_clz(s, insn);
        } else if (!extract32(insn, 21, 3) && !extract32(insn, 10, 6)) {
            handle_add(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    case 0x1b:
        handle_dp3s(s, insn);
        break;
    case 0x1e:
        if (!extract32(insn, 21, 1) && !extract32(insn, 30, 1)) {
            handle_fpfpconv(s, insn);
        } else if (!extract32(insn, 29, 3) && extract32(insn, 21, 1) &&
                   (extract32(insn, 5, 8) == 0x80)) {
            handle_fmovi(s, insn);
        } else if (extract32(insn, 21, 1) && !extract32(insn, 30, 1) &&
                   !extract32(insn, 10, 6)) {
            handle_fpintconv(s, insn);
        } else if (!extract32(insn, 29, 3) && extract32(insn, 21, 1) &&
                   (extract32(insn, 10, 6) == 0x8) && !extract32(insn, 0, 3)) {
            handle_fcmp(s, insn);
        } else if (!extract32(insn, 29, 3) && !extract32(insn, 22, 2) &&
                   extract32(insn, 21, 1) && (extract32(insn, 10, 5) == 0x10)) {
            handle_fpdp1s32(s, insn);
        } else if (!extract32(insn, 29, 3) && (extract32(insn, 22, 2) == 0x1) &&
                   extract32(insn, 21, 1) && (extract32(insn, 10, 5) == 0x10)) {
            handle_fpdp1s64(s, insn);
        } else if (!extract32(insn, 29, 3) && !extract32(insn, 22, 2) &&
                   extract32(insn, 21, 1) && (extract32(insn, 10, 2) == 0x2)) {
            handle_fpdp2s32(s, insn);
        } else {
            unallocated_encoding(s);
        }
        break;
    default:
        unallocated_encoding(s);
        break;
    }

insn_done:
    if (unlikely(s->singlestep_enabled) && (s->is_jmp == DISAS_TB_JUMP)) {
        /* go through the main loop for single step */
        s->is_jmp = DISAS_JUMP;
    }
}
