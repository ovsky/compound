//! ARM64 (A64) interpreter -- see `cpu/interpreter.h` for the contract this
//! implements and for the instruction set it covers.
//!
//! The file is split the way the interpreter runs: `decode` turns one word into a
//! flat description, `execute` applies that description, and `interpreter_run` is
//! the loop that fetches a word, asks for its description, and either traps on it,
//! refuses it, or executes it. `interpreter_probe` shares `decode` so "can this be
//! run here?" and "run it" can never disagree.

#include "cpu/interpreter.h"

#include "log.h"

#include <string.h>

// -----------------------------------------------------------------------------
// Decoding
// -----------------------------------------------------------------------------

/// One instruction, flattened. Every field is only meaningful for the operations
/// listed next to it; `decode` zeroes the whole struct first so an operation that
/// reads a field its encoding does not define reads zero rather than garbage.
typedef enum
{
    A64_OP_INVALID = 0,

    A64_OP_NOP,

    A64_OP_MOVZ,
    A64_OP_MOVN,
    A64_OP_MOVK,

    A64_OP_LOG_IMM, // opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS; bitmask holds the immediate
    A64_OP_ADD_IMM,
    A64_OP_SUB_IMM,

    A64_OP_LOG_REG, // opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS; invert = N bit
    A64_OP_ADD_REG,
    A64_OP_SUB_REG,

    A64_OP_MADD,
    A64_OP_MSUB,
    A64_OP_UDIV,
    A64_OP_SDIV,
    A64_OP_LSLV,
    A64_OP_LSRV,
    A64_OP_ASRV,

    A64_OP_CSEL,
    A64_OP_CSINC,
    A64_OP_CSINV,
    A64_OP_CSNEG,

    A64_OP_ADR,
    A64_OP_ADRP,

    A64_OP_B,
    A64_OP_BL,
    A64_OP_B_COND,
    A64_OP_CBZ,
    A64_OP_CBNZ,
    A64_OP_TBZ,
    A64_OP_TBNZ,
    A64_OP_BR,
    A64_OP_BLR,
    A64_OP_RET,

    A64_OP_LOAD,         // size + sign_extend
    A64_OP_STORE,        // size
    A64_OP_LOAD_LITERAL, // size + sign_extend
    A64_OP_LDP,          // size
    A64_OP_STP,          // size

    A64_OP_SVC,
    A64_OP_BRK,
    A64_OP_HLT,
} a64_op_t;

typedef struct
{
    a64_op_t op;

    bool sf;        ///< 1 = 64-bit operand size, 0 = 32-bit
    bool set_flags; ///< ADDS/SUBS/ANDS: update NZCV
    bool is_store;  ///< load/store: direction
    bool sign_extend; ///< load: sign-extend the value to 64 bits
    bool invert;    ///< logical-register N bit; conditional-select CSINV/CSNEG
    bool increment; ///< conditional-select CSINC/CSNEG

    uint8_t rd, rn, rm, ra, rt, rt2;
    uint8_t opc;        ///< logical family selector
    uint8_t shift;      ///< 0 LSL, 1 LSR, 2 ASR, 3 ROR
    uint8_t amount;     ///< shift amount, or move-wide bit position
    uint8_t cond;
    uint8_t size;       ///< access size in bytes

    int64_t  imm;     ///< sign-extended immediate (branch offset, address, offset)
    uint32_t uimm;    ///< zero-extended immediate (imm16, bit index)
    uint64_t bitmask; ///< decoded logical immediate
} a64_decoded_t;

/// Extracts the inclusive bit range `[hi, lo]`.
static uint32_t field(uint32_t instr, unsigned hi, unsigned lo)
{
    const unsigned width = hi - lo + 1U;
    const uint32_t mask  = (width >= 32U) ? UINT32_MAX : ((1U << width) - 1U);
    return (instr >> lo) & mask;
}

static int64_t sign_extend(uint64_t value, unsigned bits)
{
    const unsigned shift = 64U - bits;
    return (int64_t)(value << shift) >> shift;
}

static uint64_t width_mask(unsigned width)
{
    return (width >= 64U) ? UINT64_MAX : ((1ULL << width) - 1ULL);
}

/// Index of the most significant set bit, or 0 for an input of 0.
static unsigned highest_set_bit(unsigned value)
{
    unsigned position = 0;
    while (value >>= 1U)
    {
        ++position;
    }
    return position;
}

static uint64_t rotate_right(uint64_t value, unsigned amount, unsigned width)
{
    const unsigned range = width - 1U;
    amount &= range;
    return ((value >> amount) | (value << ((width - amount) & range))) & width_mask(width);
}

static uint64_t replicate(uint64_t element, unsigned esize, unsigned width)
{
    uint64_t result = 0;
    for (unsigned offset = 0; offset < width; offset += esize)
    {
        result |= element << offset;
    }
    return result;
}

/// Decodes a logical immediate (the `DecodeBitMasks` pseudocode from the ARM ARM).
///
/// Returns false for the encodings the architecture leaves undefined, so the caller
/// reports them rather than inventing a mask.
static bool decode_logical_immediate(bool sf, uint8_t n, uint8_t immr, uint8_t imms, uint64_t *out)
{
    if (n != (sf ? 1U : 0U))
    {
        return false;
    }

    const unsigned len = highest_set_bit(((unsigned)(n & 1U) << 6U) | ((unsigned)(~imms) & 0x3FU));
    if (len < 1U)
    {
        return false;
    }

    const unsigned levels = (1U << len) - 1U;
    const unsigned s      = (unsigned)imms & levels;
    const unsigned r      = (unsigned)immr & levels;

    if (s == levels)
    {
        return false;
    }

    const unsigned esize = 1U << len;
    if (esize > (sf ? 64U : 32U))
    {
        return false;
    }

    uint64_t element = (1ULL << (s + 1U)) - 1ULL;
    element = rotate_right(element, r, esize);

    const uint64_t wmask = replicate(element, esize, sf ? 64U : 32U);
    *out                 = sf ? wmask : (wmask & 0xFFFFFFFFULL);
    return true;
}

static uint64_t shift_value(uint64_t value, uint8_t shift, unsigned amount, unsigned width)
{
    amount %= width;

    switch (shift)
    {
        case 1U: // LSR
            return (value >> amount) & width_mask(width);
        case 2U: // ASR
            return (uint64_t)(sign_extend(value, width) >> amount) & width_mask(width);
        case 3U: // ROR
            return (0U == amount) ? (value & width_mask(width))
                                  : (((value >> amount) | (value << (width - amount))) & width_mask(width));
        case 0U: // LSL
        default:
            return (value << amount) & width_mask(width);
    }
}

static bool decode(uint32_t instr, a64_decoded_t *d)
{
    memset(d, 0, sizeof(*d));
    d->op = A64_OP_INVALID;

    // HINT space, `NOP` included: every hint is a no-op here.
    if ((instr & 0xFFFFF01FU) == 0xD503201FU)
    {
        d->op = A64_OP_NOP;
        return true;
    }

    // Exception generation: SVC / BRK / HLT.
    if ((instr & 0xFFE0001FU) == 0xD4000001U)
    {
        d->op   = A64_OP_SVC;
        d->uimm = field(instr, 20, 5);
        return true;
    }
    if ((instr & 0xFFE0001FU) == 0xD4200000U)
    {
        d->op   = A64_OP_BRK;
        d->uimm = field(instr, 20, 5);
        return true;
    }
    if ((instr & 0xFFE0001FU) == 0xD4400000U)
    {
        d->op   = A64_OP_HLT;
        d->uimm = field(instr, 20, 5);
        return true;
    }

    // Unconditional branch (immediate): B / BL.
    if ((instr & 0x7C000000U) == 0x14000000U)
    {
        d->op  = (0U == (instr & 0x80000000U)) ? A64_OP_B : A64_OP_BL;
        d->imm = sign_extend((uint64_t)field(instr, 25, 0) << 2U, 28U);
        return true;
    }

    // Conditional branch (immediate): B.cond.
    if ((instr & 0xFF000010U) == 0x54000000U)
    {
        d->op   = A64_OP_B_COND;
        d->cond = (uint8_t)field(instr, 3, 0);
        d->imm  = sign_extend((uint64_t)field(instr, 23, 5) << 2U, 21U);
        return true;
    }

    // Compare and branch: CBZ / CBNZ.
    if ((instr & 0x7E000000U) == 0x34000000U)
    {
        d->sf  = (0U != (instr & 0x80000000U));
        d->op  = (0U == (instr & 0x01000000U)) ? A64_OP_CBZ : A64_OP_CBNZ;
        d->rt  = (uint8_t)field(instr, 4, 0);
        d->imm = sign_extend((uint64_t)field(instr, 23, 5) << 2U, 21U);
        return true;
    }

    // Test and branch: TBZ / TBNZ.
    if ((instr & 0x7E000000U) == 0x36000000U)
    {
        d->op   = (0U == (instr & 0x01000000U)) ? A64_OP_TBZ : A64_OP_TBNZ;
        d->rt   = (uint8_t)field(instr, 4, 0);
        d->uimm = (field(instr, 31, 31) << 5U) | field(instr, 23, 19);
        d->imm  = sign_extend((uint64_t)field(instr, 18, 5) << 2U, 16U);
        return true;
    }

    // Unconditional branch (register): BR / BLR / RET.
    if (((instr & 0xFE000000U) == 0xD6000000U) && (field(instr, 20, 16) == 0x1FU) &&
        (field(instr, 15, 10) == 0U) && (field(instr, 4, 0) == 0U))
    {
        d->rn = (uint8_t)field(instr, 9, 5);
        switch (field(instr, 24, 21))
        {
            case 0U: d->op = A64_OP_BR;  return true;
            case 1U: d->op = A64_OP_BLR; return true;
            case 2U: d->op = A64_OP_RET; return true;
            default: return false;
        }
    }

    // Load register (literal).
    if ((instr & 0x3F000000U) == 0x18000000U)
    {
        const uint32_t opc = field(instr, 31, 30);
        d->rt  = (uint8_t)field(instr, 4, 0);
        d->imm = sign_extend((uint64_t)field(instr, 23, 5) << 2U, 21U);
        switch (opc)
        {
            case 0U: d->op = A64_OP_LOAD_LITERAL; d->size = 4U; d->sf = false; d->sign_extend = false; return true;
            case 1U: d->op = A64_OP_LOAD_LITERAL; d->size = 8U; d->sf = true;  d->sign_extend = false; return true;
            case 2U: d->op = A64_OP_LOAD_LITERAL; d->size = 4U; d->sf = true;  d->sign_extend = true;  return true;
            default: return false; // PRFM: no architectural effect here
        }
    }

    // Load/store register (unsigned immediate).
    if ((instr & 0x3F000000U) == 0x39000000U)
    {
        const uint32_t size  = field(instr, 31, 30);
        const uint32_t opc   = field(instr, 23, 22);
        const uint32_t imm12 = field(instr, 21, 10);

        // size 11 with opc 10/11 is PRFM, and size 10 with opc 11 is reserved;
        // neither moves a general register, so neither is modelled.
        if ((size == 3U) && (opc >= 2U))
        {
            return false;
        }
        if ((size == 2U) && (opc == 3U))
        {
            return false;
        }

        d->rn          = (uint8_t)field(instr, 9, 5);
        d->rt          = (uint8_t)field(instr, 4, 0);
        d->size        = (uint8_t)(1U << size);
        d->imm         = (int64_t)(imm12 << size);
        d->is_store    = (opc == 0U);
        d->sign_extend = (opc >= 2U);
        // opc 10 sign-extends into the 64-bit register and opc 11 into the
        // 32-bit one, so `sf` follows `opc` there; the zero-extending forms
        // reach a 64-bit register only for a 64-bit access.
        d->sf = d->sign_extend ? (opc == 2U) : (size == 3U);
        d->op = d->is_store ? A64_OP_STORE : A64_OP_LOAD;
        return true;
    }

    // Load/store pair (signed offset): LDP / STP.
    if ((instr & 0x3F800000U) == 0x29000000U)
    {
        const uint32_t opc  = field(instr, 31, 30);
        const bool     load = (0U != (instr & 0x00400000U));

        if (opc == 0U)
        {
            d->size = 4U;
            d->sf   = false;
        }
        else if (opc == 2U)
        {
            d->size = 8U;
            d->sf   = true;
        }
        else
        {
            return false; // LDPSW and the SIMD pairs
        }

        d->op       = load ? A64_OP_LDP : A64_OP_STP;
        d->is_store = !load;
        d->rn       = (uint8_t)field(instr, 9, 5);
        d->rt       = (uint8_t)field(instr, 4, 0);
        d->rt2      = (uint8_t)field(instr, 14, 10);
        d->imm      = sign_extend(field(instr, 21, 15), 7U) * (int64_t)d->size;
        return true;
    }

    // Add/subtract (immediate).
    if ((instr & 0x1F800000U) == 0x11000000U)
    {
        d->sf        = (0U != (instr & 0x80000000U));
        d->set_flags = (0U != (instr & 0x20000000U));
        d->op        = (0U != (instr & 0x40000000U)) ? A64_OP_SUB_IMM : A64_OP_ADD_IMM;
        d->rd        = (uint8_t)field(instr, 4, 0);
        d->rn        = (uint8_t)field(instr, 9, 5);
        d->uimm      = field(instr, 21, 10);
        if (0U != (instr & 0x00400000U))
        {
            d->uimm <<= 12U;
        }
        return true;
    }

    // Logical (immediate).
    if ((instr & 0x1F800000U) == 0x12000000U)
    {
        d->sf        = (0U != (instr & 0x80000000U));
        d->opc       = (uint8_t)field(instr, 30, 29);
        d->set_flags = (d->opc == 3U);
        d->rn        = (uint8_t)field(instr, 9, 5);
        d->rd        = (uint8_t)field(instr, 4, 0);

        const uint8_t n    = (uint8_t)field(instr, 22, 22);
        const uint8_t immr = (uint8_t)field(instr, 21, 16);
        const uint8_t imms = (uint8_t)field(instr, 15, 10);

        if (!decode_logical_immediate(d->sf, n, immr, imms, &d->bitmask))
        {
            return false;
        }

        d->op = A64_OP_LOG_IMM;
        return true;
    }

    // Move wide (immediate): MOVN / MOVZ / MOVK.
    {
        const uint32_t move_wide = instr & 0x7F800000U;
        if ((move_wide == 0x12800000U) || (move_wide == 0x52800000U) || (move_wide == 0x72800000U))
        {
            d->sf   = (0U != (instr & 0x80000000U));
            d->rd   = (uint8_t)field(instr, 4, 0);
            d->uimm = field(instr, 20, 5);

            const uint32_t hw = field(instr, 22, 21);
            if ((!d->sf) && (hw > 1U))
            {
                return false;
            }
            d->amount = (uint8_t)(hw * 16U);

            if (move_wide == 0x12800000U)
            {
                d->op = A64_OP_MOVN;
            }
            else if (move_wide == 0x52800000U)
            {
                d->op = A64_OP_MOVZ;
            }
            else
            {
                d->op = A64_OP_MOVK;
            }
            return true;
        }
    }

    // Logical (shifted register).
    if ((instr & 0x1F000000U) == 0x0A000000U)
    {
        d->sf        = (0U != (instr & 0x80000000U));
        d->opc       = (uint8_t)field(instr, 30, 29);
        d->set_flags = (d->opc == 3U);
        d->shift     = (uint8_t)field(instr, 23, 22);
        d->invert    = (0U != (instr & 0x00200000U));
        d->rm        = (uint8_t)field(instr, 20, 16);
        d->amount    = (uint8_t)field(instr, 15, 10);
        d->rn        = (uint8_t)field(instr, 9, 5);
        d->rd        = (uint8_t)field(instr, 4, 0);
        d->op        = A64_OP_LOG_REG;
        return true;
    }

    // Add/subtract (shifted register).
    if ((instr & 0x1F200000U) == 0x0B000000U)
    {
        d->sf        = (0U != (instr & 0x80000000U));
        d->set_flags = (0U != (instr & 0x20000000U));
        d->op        = (0U != (instr & 0x40000000U)) ? A64_OP_SUB_REG : A64_OP_ADD_REG;
        d->shift     = (uint8_t)field(instr, 23, 22);
        d->rm        = (uint8_t)field(instr, 20, 16);
        d->amount    = (uint8_t)field(instr, 15, 10);
        d->rn        = (uint8_t)field(instr, 9, 5);
        d->rd        = (uint8_t)field(instr, 4, 0);
        return true;
    }

    // Data-processing (2 source): UDIV / SDIV / LSLV / LSRV / ASRV.
    if ((instr & 0x1FE00000U) == 0x1AC00000U)
    {
        d->sf = (0U != (instr & 0x80000000U));
        d->rm = (uint8_t)field(instr, 20, 16);
        d->rn = (uint8_t)field(instr, 9, 5);
        d->rd = (uint8_t)field(instr, 4, 0);

        switch (field(instr, 15, 10))
        {
            case 0x02U: d->op = A64_OP_UDIV; return true;
            case 0x03U: d->op = A64_OP_SDIV; return true;
            case 0x08U: d->op = A64_OP_LSLV; return true;
            case 0x09U: d->op = A64_OP_LSRV; return true;
            case 0x0AU: d->op = A64_OP_ASRV; return true;
            default: return false;
        }
    }

    // Data-processing (3 source): MADD / MSUB.
    if ((instr & 0x1FE00000U) == 0x1B000000U)
    {
        if (field(instr, 23, 21) != 0U)
        {
            return false; // the long-multiply forms are not modelled
        }

        d->sf = (0U != (instr & 0x80000000U));
        d->rm = (uint8_t)field(instr, 20, 16);
        d->ra = (uint8_t)field(instr, 14, 10);
        d->rn = (uint8_t)field(instr, 9, 5);
        d->rd = (uint8_t)field(instr, 4, 0);
        d->op = (0U != (instr & 0x00008000U)) ? A64_OP_MSUB : A64_OP_MADD;
        return true;
    }

    // Conditional select: CSEL / CSINC / CSINV / CSNEG.
    if ((instr & 0x3FE00000U) == 0x1A800000U)
    {
        if (0U != (instr & 0x00000800U))
        {
            return false; // bit 11 is fixed zero
        }

        d->sf        = (0U != (instr & 0x80000000U));
        d->invert    = (0U != (instr & 0x40000000U));
        d->increment = (0U != (instr & 0x00000400U));
        d->rm        = (uint8_t)field(instr, 20, 16);
        d->cond      = (uint8_t)field(instr, 15, 12);
        d->rn        = (uint8_t)field(instr, 9, 5);
        d->rd        = (uint8_t)field(instr, 4, 0);

        if (d->invert)
        {
            d->op = d->increment ? A64_OP_CSNEG : A64_OP_CSINV;
        }
        else
        {
            d->op = d->increment ? A64_OP_CSINC : A64_OP_CSEL;
        }
        return true;
    }

    // PC-relative addressing: ADR / ADRP.
    if ((instr & 0x1F000000U) == 0x10000000U)
    {
        const uint64_t immediate = ((uint64_t)field(instr, 23, 5) << 2U) | field(instr, 30, 29);
        d->op                    = (0U == (instr & 0x80000000U)) ? A64_OP_ADR : A64_OP_ADRP;
        d->rd                    = (uint8_t)field(instr, 4, 0);
        d->imm                   = sign_extend(immediate, 21U);
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Execution
// -----------------------------------------------------------------------------

static uint64_t read_reg(const guest_state_t *state, uint8_t index, bool sp)
{
    if (index == 31U)
    {
        return sp ? state->x[31] : 0ULL;
    }
    return state->x[index];
}

static void write_reg(guest_state_t *state, uint8_t index, uint64_t value, bool sp)
{
    if (index == 31U)
    {
        if (sp)
        {
            state->x[31] = value;
        }
        return;
    }
    state->x[index] = value;
}

static void set_logic_flags(guest_state_t *state, uint64_t result, unsigned width)
{
    const uint64_t sign = 1ULL << (width - 1U);
    state->flag_n       = (0U != (result & sign)) ? 1U : 0U;
    state->flag_z       = (0U == result) ? 1U : 0U;
    state->flag_c       = 0U;
    state->flag_v       = 0U;
}

static void set_add_flags(guest_state_t *state, uint64_t a, uint64_t b, uint64_t result, unsigned width)
{
    const uint64_t keep = width_mask(width);
    const uint64_t sign = 1ULL << (width - 1U);

    a &= keep;
    b &= keep;
    result &= keep;

    state->flag_n = (0U != (result & sign)) ? 1U : 0U;
    state->flag_z = (0U == result) ? 1U : 0U;
    state->flag_c = (result < a) ? 1U : 0U;
    state->flag_v = (0U != ((~(a ^ b)) & (a ^ result) & sign)) ? 1U : 0U;
}

static void set_sub_flags(guest_state_t *state, uint64_t a, uint64_t b, uint64_t result, unsigned width)
{
    const uint64_t keep = width_mask(width);
    const uint64_t sign = 1ULL << (width - 1U);

    a &= keep;
    b &= keep;
    result &= keep;

    state->flag_n = (0U != (result & sign)) ? 1U : 0U;
    state->flag_z = (0U == result) ? 1U : 0U;
    state->flag_c = (a >= b) ? 1U : 0U;
    state->flag_v = (0U != ((a ^ b) & (a ^ result) & sign)) ? 1U : 0U;
}

static bool condition_holds(const guest_state_t *state, uint8_t cond)
{
    const bool n = state->flag_n != 0U;
    const bool z = state->flag_z != 0U;
    const bool c = state->flag_c != 0U;
    const bool v = state->flag_v != 0U;

    bool base;
    switch (cond >> 1U)
    {
        case 0U: base = z; break;             // EQ / NE
        case 1U: base = c; break;             // CS / CC
        case 2U: base = n; break;             // MI / PL
        case 3U: base = v; break;             // VS / VC
        case 4U: base = c && !z; break;       // HI / LS
        case 5U: base = (n == v); break;      // GE / LT
        case 6U: base = (n == v) && !z; break; // GT / LE
        default: return true;                  // AL / NV
    }

    return (0U != (cond & 1U)) ? !base : base;
}

static error_t load_bytes(guest_memory_t *memory, uint64_t address, unsigned size, bool sign_extend_to_64,
                          uint64_t *out)
{
    size_t         readable = 0;
    const uint8_t *host     = guest_memory_translate_read(memory, address, &readable);
    if ((NULL == host) || (readable < (size_t)size))
    {
        return POUND_ERROR_MEMORY_FAULT;
    }

    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
    {
        value |= (uint64_t)host[i] << (8U * i);
    }

    if (sign_extend_to_64 && (size < 8U))
    {
        value = (uint64_t)sign_extend(value, size * 8U);
    }

    *out = value;
    return POUND_SUCCESS;
}

static error_t store_bytes(guest_memory_t *memory, uint64_t address, unsigned size, uint64_t value)
{
    size_t   writable = 0;
    uint8_t *host     = guest_memory_translate_write(memory, address, &writable);
    if ((NULL == host) || (writable < (size_t)size))
    {
        return POUND_ERROR_MEMORY_FAULT;
    }

    for (unsigned i = 0; i < size; ++i)
    {
        host[i] = (uint8_t)(value >> (8U * i));
    }
    return POUND_SUCCESS;
}

static error_t execute(const a64_decoded_t *d, guest_state_t *state, guest_memory_t *memory)
{
    const unsigned width = d->sf ? 64U : 32U;
    const uint64_t keep  = width_mask(width);

    // Most instructions fall through to the next one; the branches overwrite this.
    uint64_t next_pc = state->pc + 4U;

    switch (d->op)
    {
        case A64_OP_NOP:
            break;

        case A64_OP_MOVZ:
            write_reg(state, d->rd, ((uint64_t)d->uimm << d->amount) & keep, false);
            break;

        case A64_OP_MOVN:
            write_reg(state, d->rd, (~((uint64_t)d->uimm << d->amount)) & keep, false);
            break;

        case A64_OP_MOVK:
        {
            const uint64_t mask = (uint64_t)0xFFFFU << d->amount;
            write_reg(state, d->rd, ((read_reg(state, d->rd, false) & ~mask) | ((uint64_t)d->uimm << d->amount)) & keep,
                      false);
            break;
        }

        case A64_OP_ADD_IMM:
        case A64_OP_SUB_IMM:
        {
            const bool     sub    = (d->op == A64_OP_SUB_IMM);
            const uint64_t a      = read_reg(state, d->rn, true) & keep;
            const uint64_t b      = (uint64_t)d->uimm & keep;
            const uint64_t result = sub ? (a - b) : (a + b);

            if (d->set_flags)
            {
                if (sub)
                {
                    set_sub_flags(state, a, b, result, width);
                }
                else
                {
                    set_add_flags(state, a, b, result, width);
                }
                write_reg(state, d->rd, result & keep, false); // Rd == 31 is ZR
            }
            else
            {
                write_reg(state, d->rd, result & keep, true); // Rd == 31 is SP
            }
            break;
        }

        case A64_OP_LOG_IMM:
        {
            const uint64_t a = read_reg(state, d->rn, false) & keep;
            const uint64_t b = d->bitmask & keep;

            uint64_t result;
            switch (d->opc)
            {
                case 1U: result = a | b; break;
                case 2U: result = a ^ b; break;
                case 0U:
                default: result = a & b; break;
            }

            if (d->set_flags)
            {
                set_logic_flags(state, result & keep, width);
            }
            write_reg(state, d->rd, result & keep, false);
            break;
        }

        case A64_OP_LOG_REG:
        {
            const uint64_t a = read_reg(state, d->rn, false) & keep;
            uint64_t       b = shift_value(read_reg(state, d->rm, false) & keep, d->shift, d->amount, width);
            if (d->invert)
            {
                b = (~b) & keep;
            }

            uint64_t result;
            switch (d->opc)
            {
                case 1U: result = a | b; break;
                case 2U: result = a ^ b; break;
                case 0U:
                default: result = a & b; break;
            }

            if (d->set_flags)
            {
                set_logic_flags(state, result & keep, width);
            }
            write_reg(state, d->rd, result & keep, false);
            break;
        }

        case A64_OP_ADD_REG:
        case A64_OP_SUB_REG:
        {
            const bool     sub    = (d->op == A64_OP_SUB_REG);
            const uint64_t a      = read_reg(state, d->rn, false) & keep;
            const uint64_t b      = shift_value(read_reg(state, d->rm, false) & keep, d->shift, d->amount, width);
            const uint64_t result = sub ? (a - b) : (a + b);

            if (d->set_flags)
            {
                if (sub)
                {
                    set_sub_flags(state, a, b, result, width);
                }
                else
                {
                    set_add_flags(state, a, b, result, width);
                }
            }
            write_reg(state, d->rd, result & keep, false); // Rd == 31 is ZR here
            break;
        }

        case A64_OP_MADD:
        case A64_OP_MSUB:
        {
            const uint64_t a       = read_reg(state, d->rn, false) & keep;
            const uint64_t b       = read_reg(state, d->rm, false) & keep;
            const uint64_t acc     = read_reg(state, d->ra, false) & keep;
            const uint64_t product = (a * b) & keep;
            const uint64_t result  = (d->op == A64_OP_MSUB) ? (acc - product) : (acc + product);
            write_reg(state, d->rd, result & keep, false);
            break;
        }

        case A64_OP_UDIV:
        {
            const uint64_t a = read_reg(state, d->rn, false) & keep;
            const uint64_t b = read_reg(state, d->rm, false) & keep;
            write_reg(state, d->rd, (0U == b) ? 0U : ((a / b) & keep), false);
            break;
        }

        case A64_OP_SDIV:
        {
            const int64_t a          = sign_extend(read_reg(state, d->rn, false) & keep, width);
            const int64_t b          = sign_extend(read_reg(state, d->rm, false) & keep, width);
            const int64_t int_min    = (64U == width) ? INT64_MIN : INT32_MIN;
            int64_t       result     = 0;

            if (0U != b)
            {
                // The one division that overflows returns the dividend, not a trap.
                result = ((int_min == a) && (-1 == b)) ? a : (a / b);
            }
            write_reg(state, d->rd, (uint64_t)result & keep, false);
            break;
        }

        case A64_OP_LSLV:
        case A64_OP_LSRV:
        case A64_OP_ASRV:
        {
            const uint64_t value  = read_reg(state, d->rn, false) & keep;
            const unsigned amount = (unsigned)(read_reg(state, d->rm, false) & (width - 1U));

            uint64_t result;
            if (d->op == A64_OP_LSLV)
            {
                result = (value << amount) & keep;
            }
            else if (d->op == A64_OP_LSRV)
            {
                result = (value >> amount) & keep;
            }
            else
            {
                result = (uint64_t)(sign_extend(value, width) >> amount) & keep;
            }
            write_reg(state, d->rd, result, false);
            break;
        }

        case A64_OP_CSEL:
        case A64_OP_CSINC:
        case A64_OP_CSINV:
        case A64_OP_CSNEG:
        {
            uint64_t result;
            if (condition_holds(state, d->cond))
            {
                result = read_reg(state, d->rn, false) & keep;
            }
            else
            {
                uint64_t other = read_reg(state, d->rm, false) & keep;
                if (d->invert)
                {
                    other = (~other) & keep;
                }
                if (d->increment)
                {
                    other = (other + 1U) & keep;
                }
                result = other;
            }
            write_reg(state, d->rd, result, false);
            break;
        }

        case A64_OP_ADR:
            write_reg(state, d->rd, (uint64_t)((int64_t)state->pc + d->imm), false);
            break;

        case A64_OP_ADRP:
            write_reg(state, d->rd,
                      (uint64_t)(((int64_t)state->pc & ~(int64_t)0xFFFU) + (d->imm << 12U)), false);
            break;

        case A64_OP_B:
            next_pc = (uint64_t)((int64_t)state->pc + d->imm);
            break;

        case A64_OP_BL:
            write_reg(state, 30U, state->pc + 4U, false);
            next_pc = (uint64_t)((int64_t)state->pc + d->imm);
            break;

        case A64_OP_B_COND:
            if (condition_holds(state, d->cond))
            {
                next_pc = (uint64_t)((int64_t)state->pc + d->imm);
            }
            break;

        case A64_OP_CBZ:
        case A64_OP_CBNZ:
        {
            const uint64_t value = read_reg(state, d->rt, false) & keep;
            const bool     zero  = (0U == value);
            const bool     take  = (d->op == A64_OP_CBZ) ? zero : !zero;
            if (take)
            {
                next_pc = (uint64_t)((int64_t)state->pc + d->imm);
            }
            break;
        }

        case A64_OP_TBZ:
        case A64_OP_TBNZ:
        {
            const uint64_t value = read_reg(state, d->rt, false);
            const bool     bit   = (0U != ((value >> d->uimm) & 1U));
            const bool     take  = (d->op == A64_OP_TBZ) ? !bit : bit;
            if (take)
            {
                next_pc = (uint64_t)((int64_t)state->pc + d->imm);
            }
            break;
        }

        case A64_OP_BR:
            next_pc = read_reg(state, d->rn, false);
            break;

        case A64_OP_BLR:
            write_reg(state, 30U, state->pc + 4U, false);
            next_pc = read_reg(state, d->rn, false);
            break;

        case A64_OP_RET:
            next_pc = read_reg(state, d->rn, false);
            break;

        case A64_OP_LOAD:
        case A64_OP_STORE:
        {
            const uint64_t address = read_reg(state, d->rn, true) + (uint64_t)d->imm;

            if (d->is_store)
            {
                const error_t status = store_bytes(memory, address, d->size, read_reg(state, d->rt, false));
                if (POUND_SUCCESS != status)
                {
                    return status;
                }
            }
            else
            {
                uint64_t      value  = 0;
                const error_t status = load_bytes(memory, address, d->size, d->sign_extend, &value);
                if (POUND_SUCCESS != status)
                {
                    return status;
                }
                write_reg(state, d->rt, value & (d->sf ? UINT64_MAX : 0xFFFFFFFFULL), false);
            }
            break;
        }

        case A64_OP_LOAD_LITERAL:
        {
            const uint64_t address = (uint64_t)((int64_t)state->pc + d->imm);
            uint64_t       value   = 0;
            const error_t  status  = load_bytes(memory, address, d->size, d->sign_extend, &value);
            if (POUND_SUCCESS != status)
            {
                return status;
            }
            write_reg(state, d->rt, value & (d->sf ? UINT64_MAX : 0xFFFFFFFFULL), false);
            break;
        }

        case A64_OP_LDP:
        case A64_OP_STP:
        {
            const uint64_t base = read_reg(state, d->rn, true) + (uint64_t)d->imm;

            if (d->is_store)
            {
                error_t status = store_bytes(memory, base, d->size, read_reg(state, d->rt, false));
                if (POUND_SUCCESS != status)
                {
                    return status;
                }
                status = store_bytes(memory, base + d->size, d->size, read_reg(state, d->rt2, false));
                if (POUND_SUCCESS != status)
                {
                    return status;
                }
            }
            else
            {
                uint64_t first  = 0;
                uint64_t second = 0;

                error_t status = load_bytes(memory, base, d->size, false, &first);
                if (POUND_SUCCESS != status)
                {
                    return status;
                }
                status = load_bytes(memory, base + d->size, d->size, false, &second);
                if (POUND_SUCCESS != status)
                {
                    return status;
                }
                const uint64_t mask = d->sf ? UINT64_MAX : 0xFFFFFFFFULL;
                write_reg(state, d->rt, first & mask, false);
                write_reg(state, d->rt2, second & mask, false);
            }
            break;
        }

        default:
            // Traps are filtered before `execute` and `A64_OP_INVALID` never
            // reaches here, so this is unreachable for a decoded instruction.
            return POUND_ERROR_UNSUPPORTED_INSTRUCTION;
    }

    state->pc = next_pc;
    return POUND_SUCCESS;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

error_t interpreter_run(guest_state_t *POUND_RESTRICT state, guest_memory_t *POUND_RESTRICT memory, uint32_t budget,
                        uint32_t *POUND_RESTRICT executed, interpreter_trap_t *POUND_RESTRICT trap_out)
{
    if ((NULL == state) || (NULL == memory) || (NULL == executed))
    {
        POUND_LOG_ERROR(&thread_logger, "Aborting function: interpreter_run was handed a NULL argument.");
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    *executed = 0U;
    if (NULL != trap_out)
    {
        memset(trap_out, 0, sizeof(*trap_out));
    }

    for (uint32_t i = 0U; i < budget; ++i)
    {
        // Fetch is gated on execute permission, not read permission: a region
        // the guest may read but not run from must refuse its bytes here, the
        // way a hardware MMU refuses an instruction fetch from a non-executable
        // page. With no region table installed this is the same pointer the
        // read translation would have produced.
        size_t                executable = 0;
        const uint8_t *const host = guest_memory_translate_execute(memory, state->pc, &executable);
        if ((NULL == host) || (executable < 4U))
        {
            return POUND_ERROR_MEMORY_FAULT;
        }

        const uint32_t instr = (uint32_t)host[0] | ((uint32_t)host[1] << 8U) | ((uint32_t)host[2] << 16U) |
                               ((uint32_t)host[3] << 24U);

        a64_decoded_t decoded;
        if (!decode(instr, &decoded))
        {
            if (NULL != trap_out)
            {
                trap_out->kind  = INTERPRETER_TRAP_UNDEFINED;
                trap_out->pc    = state->pc;
                trap_out->instr = instr;
            }
            return POUND_SUCCESS;
        }

        if ((A64_OP_SVC == decoded.op) || (A64_OP_BRK == decoded.op) || (A64_OP_HLT == decoded.op))
        {
            if (NULL != trap_out)
            {
                // `SVC` is the syscall path; `BRK` and `HLT` are both deliberate
                // halts for a debugger, so they share one kind.
                trap_out->kind = (A64_OP_SVC == decoded.op) ? INTERPRETER_TRAP_SVC : INTERPRETER_TRAP_BREAKPOINT;

                trap_out->pc    = state->pc;
                trap_out->instr = instr;
                trap_out->imm   = decoded.uimm;
            }
            return POUND_SUCCESS;
        }

        const error_t status = execute(&decoded, state, memory);
        if (POUND_SUCCESS != status)
        {
            return status;
        }

        *executed += 1U;
    }

    return POUND_SUCCESS;
}

int interpreter_probe(uint32_t instr)
{
    a64_decoded_t decoded;
    return decode(instr, &decoded) ? 1 : 0;
}

/*** end of file ***/
