// Copyright 2026 Dekui contributors
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Inline helpers used by AOT-generated blocks. Each helper reproduces what
// Dynarmic's arm64 backend emits for the corresponding IR operation, so the
// generated code requires an AArch64 host. Floating point uses inline assembly
// for the same reason Dynarmic does: the host FPCR (set by the dispatcher from
// the guest FPSCR mode) then gives bit-identical results, and the compiler can
// neither fold nor reorder the operations.

#include <cstdint>
#include <cstring>
#include <type_traits>
#include "core/arm/aot/abi.h"

#if !defined(__aarch64__)
#error "AOT-generated guest code requires an AArch64 host"
#endif

#define DEKUI_AOT_INLINE [[gnu::always_inline]] inline
#define DEKUI_AOT_MUSTTAIL [[clang::musttail]]

namespace DekuiAot::Rt {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using Abi::State;

// ---------------------------------------------------------------------------
// Memory. The page table holds host pointers to 4 KiB guest pages; a null
// entry (MMIO, rasterizer-cached memory) and page-crossing accesses go
// through the emulator's callbacks.

template <typename T>
DEKUI_AOT_INLINE T* PagePointer(State* s, u32 vaddr) {
    u8* const page = s->env->page_table[vaddr >> 12];
    if (page && (vaddr & 0xFFF) <= 0x1000 - sizeof(T))
        return reinterpret_cast<T*>(page + (vaddr & 0xFFF));
    return nullptr;
}

#define DEKUI_AOT_MEMORY(BITS)                                                                     \
    DEKUI_AOT_INLINE u##BITS Read##BITS(State* s, u32 vaddr) {                                     \
        if (u8* p = reinterpret_cast<u8*>(PagePointer<u##BITS>(s, vaddr))) {                       \
            u##BITS value;                                                                         \
            std::memcpy(&value, p, sizeof(value));                                                 \
            return value;                                                                          \
        }                                                                                          \
        return s->env->read##BITS(s, vaddr);                                                       \
    }                                                                                              \
    DEKUI_AOT_INLINE void Write##BITS(State* s, u32 vaddr, u##BITS value) {                        \
        if (u8* p = reinterpret_cast<u8*>(PagePointer<u##BITS>(s, vaddr))) {                       \
            std::memcpy(p, &value, sizeof(value));                                                 \
            return;                                                                                \
        }                                                                                          \
        s->env->write##BITS(s, vaddr, value);                                                      \
    }
DEKUI_AOT_MEMORY(8)
DEKUI_AOT_MEMORY(16)
DEKUI_AOT_MEMORY(32)
DEKUI_AOT_MEMORY(64)
#undef DEKUI_AOT_MEMORY

// ---------------------------------------------------------------------------
// Extended registers: S<n> is ext_regs[n], D<n> is ext_regs[2n..2n+1].

DEKUI_AOT_INLINE u64 GetD(const State* s, unsigned index) {
    u64 value;
    std::memcpy(&value, &s->ext_regs[index], sizeof(value));
    return value;
}
DEKUI_AOT_INLINE void SetD(State* s, unsigned index, u64 value) {
    std::memcpy(&s->ext_regs[index], &value, sizeof(value));
}

// A32GetCpsr as Dynarmic's arm64 backend composes it: unlike
// A32JitState::Cpsr() it does not include the IT bits.
DEKUI_AOT_INLINE u32 GetCpsr(const State* s) {
    const u32 ge = s->cpsr_ge;
    return s->cpsr_nzcv | s->cpsr_q | s->cpsr_jaifm |
           (ge >> 31 & 1) << 19 | (ge >> 23 & 1) << 18 | (ge >> 15 & 1) << 17 | (ge >> 7 & 1) << 16 |
           (s->upper_location_descriptor >> 1 & 1) << 9 | (s->upper_location_descriptor & 1) << 5;
}

// ---------------------------------------------------------------------------
// Integer arithmetic with ARM flags (NZCV in bits 31..28).

struct AddResult {
    u32 value;
    u32 nzcv;
};

DEKUI_AOT_INLINE AddResult AddWithCarry(u32 a, u32 b, bool carry_in) {
    const u64 wide = u64(a) + u64(b) + u64(carry_in);
    const u32 result = static_cast<u32>(wide);
    const u32 n = result & 0x80000000;
    const u32 z = result == 0 ? 0x40000000 : 0;
    const u32 c = (wide >> 32) ? 0x20000000 : 0;
    const u32 v = ((~(a ^ b) & (a ^ result)) >> 31) ? 0x10000000 : 0;
    return {result, n | z | c | v};
}

DEKUI_AOT_INLINE u32 NZ(u32 value) {
    return (value & 0x80000000) | (value == 0 ? 0x40000000 : 0);
}
DEKUI_AOT_INLINE u32 NZ64(u64 value) {
    return (value >> 63 ? 0x80000000 : 0) | (value == 0 ? 0x40000000 : 0);
}

DEKUI_AOT_INLINE bool ConditionPassed(u32 nzcv, unsigned cond) {
    const bool n = nzcv >> 31 & 1, z = nzcv >> 30 & 1, c = nzcv >> 29 & 1, v = nzcv >> 28 & 1;
    switch (cond) {
    case 0: return z;               // EQ
    case 1: return !z;              // NE
    case 2: return c;               // CS
    case 3: return !c;              // CC
    case 4: return n;               // MI
    case 5: return !n;              // PL
    case 6: return v;               // VS
    case 7: return !v;              // VC
    case 8: return c && !z;         // HI
    case 9: return !c || z;         // LS
    case 10: return n == v;         // GE
    case 11: return n != v;         // LT
    case 12: return !z && n == v;   // GT
    case 13: return z || n != v;    // LE
    default: return true;           // AL, NV
    }
}

// Shifts with ARM shifter carry-out. Amounts are the full 8-bit register value.
DEKUI_AOT_INLINE u32 Lsl(u32 v, u8 shift) {
    return shift >= 32 ? 0 : v << shift;
}
DEKUI_AOT_INLINE bool LslCarry(u32 v, u8 shift, bool carry_in) {
    if (shift == 0) return carry_in;
    if (shift < 32) return (v >> (32 - shift)) & 1;
    if (shift == 32) return v & 1;
    return false;
}
DEKUI_AOT_INLINE u32 Lsr(u32 v, u8 shift) {
    return shift >= 32 ? 0 : v >> shift;
}
DEKUI_AOT_INLINE bool LsrCarry(u32 v, u8 shift, bool carry_in) {
    if (shift == 0) return carry_in;
    if (shift <= 32) return (v >> (shift - 1)) & 1;
    return false;
}
DEKUI_AOT_INLINE u32 Asr(u32 v, u8 shift) {
    return static_cast<u32>(static_cast<s32>(v) >> (shift >= 31 ? 31 : shift));
}
DEKUI_AOT_INLINE bool AsrCarry(u32 v, u8 shift, bool carry_in) {
    if (shift == 0) return carry_in;
    if (shift <= 32) return (static_cast<s32>(v) >> (shift - 1)) & 1;
    return v >> 31;
}
DEKUI_AOT_INLINE u32 Ror(u32 v, u8 shift) {
    const unsigned amount = shift & 31;
    return amount == 0 ? v : (v >> amount) | (v << (32 - amount));
}
DEKUI_AOT_INLINE bool RorCarry(u32 v, u8 shift, bool carry_in) {
    if (shift == 0) return carry_in;
    return Ror(v, shift) >> 31;
}
DEKUI_AOT_INLINE u64 Lsr64(u64 v, u8 shift) {
    return shift >= 64 ? 0 : v >> shift;
}
DEKUI_AOT_INLINE u64 Lsl64(u64 v, u8 shift) {
    return shift >= 64 ? 0 : v << shift;
}
DEKUI_AOT_INLINE u64 Asr64(u64 v, u8 shift) {
    return static_cast<u64>(static_cast<s64>(v) >> (shift >= 63 ? 63 : shift));
}

DEKUI_AOT_INLINE u32 Clz(u32 v) {
    return v == 0 ? 32 : static_cast<u32>(__builtin_clz(v));
}

// USAT/SSAT. `overflow` is the Q-flag contribution.
DEKUI_AOT_INLINE u32 UnsignedSaturation(u32 value, unsigned bits, bool& overflow) {
    const s32 v = static_cast<s32>(value);
    const s64 max = (s64(1) << bits) - 1;
    overflow = v < 0 || v > max;
    return v < 0 ? 0 : (v > max ? static_cast<u32>(max) : value);
}
DEKUI_AOT_INLINE u32 SignedSaturation(u32 value, unsigned bits, bool& overflow) {
    const s64 v = static_cast<s32>(value);
    const s64 max = (s64(1) << (bits - 1)) - 1;
    const s64 min = -(s64(1) << (bits - 1));
    overflow = v < min || v > max;
    return static_cast<u32>(v < min ? min : (v > max ? max : v));
}

// Parallel (SIMD-within-a-register) helpers. GE is returned as Dynarmic's
// byte mask: 0xFF per byte for which the lane result set GE.
template <typename Lane, typename Wide, typename Op>
DEKUI_AOT_INLINE u32 Packed(u32 a, u32 b, u32* ge, Op op) {
    constexpr unsigned bits = sizeof(Lane) * 8;
    constexpr unsigned lanes = 32 / bits;
    u32 result = 0, ge_mask = 0;
    for (unsigned i = 0; i < lanes; ++i) {
        const Wide x = static_cast<Lane>(a >> (i * bits));
        const Wide y = static_cast<Lane>(b >> (i * bits));
        bool lane_ge = false;
        const Wide r = op(x, y, lane_ge);
        result |= (static_cast<u32>(r) & ((u64(1) << bits) - 1)) << (i * bits);
        if (lane_ge)
            ge_mask |= ((u64(1) << bits) - 1) << (i * bits);
    }
    if (ge)
        *ge = ge_mask;
    return result;
}

inline u32 ReadHostFpsr();
inline void WriteHostFpsr(u32 value);

/// Packed op whose lanes saturate. Clang vectorizes those into NEON
/// SQADD/UQSUB..., which set FPSR.QC; the host FPSR carries the guest's FPSCR
/// flags and the ARM instructions set none. The empty asm statements tie the
/// computation between saving and restoring FPSR.
template <typename Lane, typename Wide, typename Op>
DEKUI_AOT_INLINE u32 PackedSaturating(u32 a, u32 b, Op op) {
    const u32 fpsr = ReadHostFpsr();
    asm volatile("" : "+r"(a), "+r"(b));
    u32 result = Packed<Lane, Wide>(a, b, nullptr, op);
    asm volatile("" : "+r"(result));
    WriteHostFpsr(fpsr);
    return result;
}

template <typename T>
DEKUI_AOT_INLINE T Saturate(s64 v) {
    constexpr s64 max = s64(T(~T(0)) > 0 ? T(~T(0)) : (u64(1) << (sizeof(T) * 8 - 1)) - 1);
    constexpr s64 min = T(~T(0)) > 0 ? 0 : -(s64(1) << (sizeof(T) * 8 - 1));
    return static_cast<T>(v < min ? min : (v > max ? max : v));
}

// ---------------------------------------------------------------------------
// Floating point. Values travel as raw bits (u32 single, u64 double).

#define DEKUI_AOT_FP2(NAME, INSN)                                                                  \
    DEKUI_AOT_INLINE u32 NAME##32(u32 a, u32 b) {                                                  \
        float x, y, r;                                                                             \
        std::memcpy(&x, &a, 4);                                                                    \
        std::memcpy(&y, &b, 4);                                                                    \
        asm volatile(INSN " %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y));                           \
        u32 out;                                                                                   \
        std::memcpy(&out, &r, 4);                                                                  \
        return out;                                                                                \
    }                                                                                              \
    DEKUI_AOT_INLINE u64 NAME##64(u64 a, u64 b) {                                                  \
        double x, y, r;                                                                            \
        std::memcpy(&x, &a, 8);                                                                    \
        std::memcpy(&y, &b, 8);                                                                    \
        asm volatile(INSN " %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y));                           \
        u64 out;                                                                                   \
        std::memcpy(&out, &r, 8);                                                                  \
        return out;                                                                                \
    }
DEKUI_AOT_FP2(FPAdd, "fadd")
DEKUI_AOT_FP2(FPSub, "fsub")
DEKUI_AOT_FP2(FPMul, "fmul")
DEKUI_AOT_FP2(FPDiv, "fdiv")
DEKUI_AOT_FP2(FPMax, "fmax")
DEKUI_AOT_FP2(FPMin, "fmin")
DEKUI_AOT_FP2(FPMaxNumeric, "fmaxnm")
DEKUI_AOT_FP2(FPMinNumeric, "fminnm")
DEKUI_AOT_FP2(FPMulX, "fmulx")
#undef DEKUI_AOT_FP2

DEKUI_AOT_INLINE u32 FPSqrt32(u32 a) {
    float x, r;
    std::memcpy(&x, &a, 4);
    asm volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    u32 out;
    std::memcpy(&out, &r, 4);
    return out;
}
DEKUI_AOT_INLINE u64 FPSqrt64(u64 a) {
    double x, r;
    std::memcpy(&x, &a, 8);
    asm volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
    u64 out;
    std::memcpy(&out, &r, 8);
    return out;
}
DEKUI_AOT_INLINE u32 FPMulAdd32(u32 addend, u32 a, u32 b) {
    float z, x, y, r;
    std::memcpy(&z, &addend, 4);
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    asm volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(x), "w"(y), "w"(z));
    u32 out;
    std::memcpy(&out, &r, 4);
    return out;
}
DEKUI_AOT_INLINE u64 FPMulAdd64(u64 addend, u64 a, u64 b) {
    double z, x, y, r;
    std::memcpy(&z, &addend, 8);
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    asm volatile("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(x), "w"(y), "w"(z));
    u64 out;
    std::memcpy(&out, &r, 8);
    return out;
}

// FABS/FNEG only touch the sign bit, NaNs included.
DEKUI_AOT_INLINE u32 FPAbs32(u32 a) { return a & 0x7FFFFFFF; }
DEKUI_AOT_INLINE u64 FPAbs64(u64 a) { return a & 0x7FFFFFFFFFFFFFFFull; }
DEKUI_AOT_INLINE u32 FPNeg32(u32 a) { return a ^ 0x80000000; }
DEKUI_AOT_INLINE u64 FPNeg64(u64 a) { return a ^ 0x8000000000000000ull; }

DEKUI_AOT_INLINE u32 FPCompare32(u32 a, u32 b, bool exc_on_qnan) {
    float x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    u64 nzcv;
    if (exc_on_qnan)
        asm volatile("fcmpe %s1, %s2\n\tmrs %0, nzcv" : "=r"(nzcv) : "w"(x), "w"(y) : "cc");
    else
        asm volatile("fcmp %s1, %s2\n\tmrs %0, nzcv" : "=r"(nzcv) : "w"(x), "w"(y) : "cc");
    return static_cast<u32>(nzcv);
}
DEKUI_AOT_INLINE u32 FPCompare64(u64 a, u64 b, bool exc_on_qnan) {
    double x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    u64 nzcv;
    if (exc_on_qnan)
        asm volatile("fcmpe %d1, %d2\n\tmrs %0, nzcv" : "=r"(nzcv) : "w"(x), "w"(y) : "cc");
    else
        asm volatile("fcmp %d1, %d2\n\tmrs %0, nzcv" : "=r"(nzcv) : "w"(x), "w"(y) : "cc");
    return static_cast<u32>(nzcv);
}

DEKUI_AOT_INLINE u64 FPSingleToDouble(u32 a) {
    float x;
    double r;
    std::memcpy(&x, &a, 4);
    asm volatile("fcvt %d0, %s1" : "=w"(r) : "w"(x));
    u64 out;
    std::memcpy(&out, &r, 8);
    return out;
}
DEKUI_AOT_INLINE u32 FPDoubleToSingle(u64 a) {
    double x;
    float r;
    std::memcpy(&x, &a, 8);
    asm volatile("fcvt %s0, %d1" : "=w"(r) : "w"(x));
    u32 out;
    std::memcpy(&out, &r, 4);
    return out;
}

// Dynarmic FP::RoundingMode values.
enum Rounding : unsigned { TieEven = 0, PlusInf = 1, MinusInf = 2, Zero = 3, TieAway = 4 };

// Float -> fixed point. Rounding other than Zero only occurs with fbits == 0.
#define DEKUI_AOT_TO_FIXED(NAME, SRC, SREG, DREG, SIGN)                                            \
    template <unsigned fbits, unsigned rounding>                                                   \
    DEKUI_AOT_INLINE u32 NAME(SRC a) {                                                             \
        using F = std::conditional_t<sizeof(SRC) == 4, float, double>;                             \
        F x;                                                                                       \
        std::memcpy(&x, &a, sizeof(a));                                                            \
        u32 r;                                                                                     \
        if constexpr (rounding == Zero && fbits != 0)                                              \
            asm volatile("fcvtz" SIGN " %w0, %" SREG "1, %2" : "=r"(r) : "w"(x), "i"(fbits));     \
        else if constexpr (rounding == Zero)                                                       \
            asm volatile("fcvtz" SIGN " %w0, %" SREG "1" : "=r"(r) : "w"(x));                       \
        else if constexpr (rounding == TieEven)                                                    \
            asm volatile("fcvtn" SIGN " %w0, %" SREG "1" : "=r"(r) : "w"(x));                       \
        else if constexpr (rounding == PlusInf)                                                    \
            asm volatile("fcvtp" SIGN " %w0, %" SREG "1" : "=r"(r) : "w"(x));                       \
        else if constexpr (rounding == MinusInf)                                                   \
            asm volatile("fcvtm" SIGN " %w0, %" SREG "1" : "=r"(r) : "w"(x));                       \
        else                                                                                       \
            asm volatile("fcvta" SIGN " %w0, %" SREG "1" : "=r"(r) : "w"(x));                       \
        return r;                                                                                  \
    }
DEKUI_AOT_TO_FIXED(FPSingleToFixedS32, u32, "s", "w", "s")
DEKUI_AOT_TO_FIXED(FPSingleToFixedU32, u32, "s", "w", "u")
DEKUI_AOT_TO_FIXED(FPDoubleToFixedS32, u64, "d", "w", "s")
DEKUI_AOT_TO_FIXED(FPDoubleToFixedU32, u64, "d", "w", "u")
#undef DEKUI_AOT_TO_FIXED

// Fixed point -> float in the given rounding mode. Dynarmic switches FPCR
// only when the requested mode differs from the block's (`block_rmode`).
DEKUI_AOT_INLINE u64 SwapRounding(unsigned rounding) {
    u64 fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    // Dynarmic RoundingMode -> FPCR.RMode: TieEven 0, PlusInf 1, MinusInf 2, Zero 3.
    asm volatile("msr fpcr, %0" ::"r"((fpcr & ~u64(3 << 22)) | (u64(rounding & 3) << 22)));
    return fpcr;
}
DEKUI_AOT_INLINE void RestoreFpcr(u64 fpcr) {
    asm volatile("msr fpcr, %0" ::"r"(fpcr));
}

#define DEKUI_AOT_FROM_FIXED(NAME, DST, DREG, SIGN)                                                \
    template <unsigned fbits, unsigned rounding, unsigned block_rmode>                             \
    DEKUI_AOT_INLINE DST NAME(u32 a) {                                                             \
        using F = std::conditional_t<sizeof(DST) == 4, float, double>;                             \
        F r;                                                                                       \
        u64 saved = 0;                                                                             \
        if constexpr (rounding != block_rmode)                                                     \
            saved = SwapRounding(rounding);                                                        \
        if constexpr (fbits != 0)                                                                  \
            asm volatile(SIGN "cvtf %" DREG "0, %w1, %2" : "=w"(r) : "r"(a), "i"(fbits));          \
        else                                                                                       \
            asm volatile(SIGN "cvtf %" DREG "0, %w1" : "=w"(r) : "r"(a));                           \
        if constexpr (rounding != block_rmode)                                                     \
            RestoreFpcr(saved);                                                                    \
        DST out;                                                                                   \
        std::memcpy(&out, &r, sizeof(out));                                                        \
        return out;                                                                                \
    }
DEKUI_AOT_FROM_FIXED(FPFixedS32ToSingle, u32, "s", "s")
DEKUI_AOT_FROM_FIXED(FPFixedU32ToSingle, u32, "s", "u")
DEKUI_AOT_FROM_FIXED(FPFixedS32ToDouble, u64, "d", "s")
DEKUI_AOT_FROM_FIXED(FPFixedU32ToDouble, u64, "d", "u")
#undef DEKUI_AOT_FROM_FIXED

// Host FPSR holds the guest's cumulative FP exception flags while AOT code
// runs (the dispatcher loads and stores it, as Dynarmic's FpsrManager does).
inline u32 ReadHostFpsr() {
    u64 fpsr;
    asm volatile("mrs %0, fpsr" : "=r"(fpsr));
    return static_cast<u32>(fpsr);
}
inline void WriteHostFpsr(u32 value) {
    asm volatile("msr fpsr, %0" ::"r"(u64(value)));
}

} // namespace DekuiAot::Rt
