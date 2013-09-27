/*
 *  AArch64 specific helpers
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

#include "cpu.h"
#include "exec/gdbstub.h"
#include "helper.h"
#include "qemu/host-utils.h"
#include "sysemu/sysemu.h"
#include "qemu/bitops.h"

uint32_t HELPER(pstate_add)(uint32_t pstate, uint64_t a1, uint64_t a2,
                            uint64_t ar)
{
    int64_t s1 = a1;
    int64_t s2 = a2;
    int64_t sr = ar;

    pstate &= ~(PSTATE_N | PSTATE_Z | PSTATE_C | PSTATE_V);

    if (sr < 0) {
        pstate |= PSTATE_N;
    }

    if (!ar) {
        pstate |= PSTATE_Z;
    }

    if (ar && (ar < a1)) {
        pstate |= PSTATE_C;
    }

    if ((s1 > 0 && s2 > 0 && sr < 0) ||
        (s1 < 0 && s2 < 0 && sr > 0)) {
        pstate |= PSTATE_V;
    }

    return pstate;
}

uint32_t HELPER(pstate_add32)(uint32_t pstate, uint64_t x1, uint64_t x2,
                              uint64_t xr)
{
    uint32_t a1 = x1;
    uint32_t a2 = x2;
    uint32_t ar = xr;

    int32_t s1 = a1;
    int32_t s2 = a2;
    int32_t sr = ar;

    pstate &= ~(PSTATE_N | PSTATE_Z | PSTATE_C | PSTATE_V);

    if (sr < 0) {
        pstate |= PSTATE_N;
    }

    if (!ar) {
        pstate |= PSTATE_Z;
    }

    if (ar && (ar < a1)) {
        pstate |= PSTATE_C;
    }

    if ((s1 > 0 && s2 > 0 && sr < 0) ||
        (s1 < 0 && s2 < 0 && sr > 0)) {
        pstate |= PSTATE_V;
    }

    return pstate;
}

uint32_t HELPER(pstate_sub)(uint32_t pstate, uint64_t a1, uint64_t a2,
                            uint64_t ar)
{
    int64_t sr = ar;
    int64_t s1 = a1;
    int64_t s2 = a2;

    pstate = helper_pstate_add(pstate, a1, a2, ar);

    pstate &= ~(PSTATE_C | PSTATE_V);

    if (a2 <= a1) {
        pstate |= PSTATE_C;
    }

    /* XXX check if this is the only special case */
    if ((!a1 && a2 == 0x8000000000000000ULL) ||
        (s1 > 0 && s2 < 0 && sr < 0) ||
        (s1 < 0 && s2 > 0 && sr > 0)) {
        pstate |= PSTATE_V;
    }

    return pstate;
}

uint32_t HELPER(pstate_sub32)(uint32_t pstate, uint64_t x1, uint64_t x2,
                              uint64_t xr)
{
    uint32_t a1 = x1;
    uint32_t a2 = x2;
    uint32_t ar = xr;
    int32_t sr = ar;
    int32_t s1 = a1;
    int32_t s2 = a2;

    pstate = helper_pstate_add32(pstate, a1, a2, ar);

    pstate &= ~(PSTATE_C | PSTATE_V);

    if (a2 <= a1) {
        pstate |= PSTATE_C;
    }

    if ((!a1 && a2 == 0x80000000ULL) ||
        (s1 > 0 && s2 < 0 && sr < 0) ||
        (s1 < 0 && s2 > 0 && sr > 0)) {
        pstate |= PSTATE_V;
    }

    return pstate;
}

uint64_t HELPER(sign_extend)(uint64_t x, uint64_t is_signed, uint64_t mask)
{
    if (x & is_signed) {
        x |= mask;
    }

    return x;
}

uint32_t HELPER(cond)(uint32_t pstate, uint32_t cond)
{
    uint32_t r;

    switch (cond >> 1) {
    case 0:
        r = pstate & PSTATE_Z;
        break;
    case 1:
        r = pstate & PSTATE_C;
        break;
    case 2:
        r = pstate & PSTATE_N;
        break;
    case 3:
        r = pstate & PSTATE_V;
        break;
    case 4:
        r = (pstate & PSTATE_C) && !(pstate & PSTATE_Z);
        break;
    case 5:
        r = (((pstate & PSTATE_N) ? 1 : 0) == ((pstate & PSTATE_V) ? 1 : 0));
        break;
    case 6:
        r = (((pstate & PSTATE_N) ? 1 : 0) == ((pstate & PSTATE_V) ? 1 : 0))
               && !(pstate & PSTATE_Z);
        break;
    case 7:
    default:
        /* ALWAYS */
        r = 1;
        break;
    }

    if ((cond & 1) && (cond != 0xf)) {
        r = !r;
    }

    return !!r;
}

static int get_bits(uint32_t inst, int start, int len)
{
    return (inst >> start) & ((1 << len) - 1);
}

uint64_t HELPER(cinc)(uint32_t pstate, uint32_t insn, uint64_t n, uint64_t m)
{
    bool else_inc = get_bits(insn, 10, 1);
    int cond = get_bits(insn, 12, 4);
    bool else_inv = get_bits(insn, 30, 1);
    bool is_32bit = !get_bits(insn, 31, 1);
    uint64_t r;

    if (helper_cond(pstate, cond)) {
        r = n;
        goto out;
    }

    r = m;
    if (else_inv) {
        r = ~r;
    }
    if (else_inc) {
        r++;
    }

out:
    if (is_32bit) {
        r = (uint32_t)r;
    }

    return r;
}

uint64_t HELPER(udiv64)(uint64_t num, uint64_t den)
{
    if (den == 0)
      return 0;
    return num / den;
}

int64_t HELPER(sdiv64)(int64_t num, int64_t den)
{
    if (den == 0)
      return 0;
    if (num == LLONG_MIN && den == -1)
      return LLONG_MIN;
    return num / den;
}

uint64_t HELPER(rbit64)(uint64_t x)
{
    x =  ((x & 0xff00000000000000ULL) >> 56)
       | ((x & 0x00ff000000000000ULL) >> 40)
       | ((x & 0x0000ff0000000000ULL) >> 24)
       | ((x & 0x000000ff00000000ULL) >> 8)
       | ((x & 0x00000000ff000000ULL) << 8)
       | ((x & 0x0000000000ff0000ULL) << 24)
       | ((x & 0x000000000000ff00ULL) << 40)
       | ((x & 0x00000000000000ffULL) << 56);
    x =  ((x & 0xf0f0f0f0f0f0f0f0ULL) >> 4)
       | ((x & 0x0f0f0f0f0f0f0f0fULL) << 4);
    x =  ((x & 0x8888888888888888ULL) >> 3)
       | ((x & 0x4444444444444444ULL) >> 1)
       | ((x & 0x2222222222222222ULL) << 1)
       | ((x & 0x1111111111111111ULL) << 3);
    return x;
}
