#ifndef POUND_INTERPRETER_H
#define POUND_INTERPRETER_H

#include <stdint.h>
#include "core/guest_state.h"
#include "core/memory/guest_memory.h"

typedef enum {
    INTERPRETER_TRAP_SVC   = 1,
    INTERPRETER_TRAP_BREAKPOINT,
    INTERPRETER_TRAP_UNDEFINED,
} interpreter_trap_kind_t;

typedef struct {
    interpreter_trap_kind_t kind;
    uint64_t                pc;
    uint32_t                instr;
    uint64_t                imm;
} interpreter_trap_t;

/* ——— Public API ———————————————————————————————— */

error_t interpreter_run(guest_state_t *state, guest_memory_t *memory,
                        uint32_t budget, uint32_t *executed,
                        interpreter_trap_t *trap_out);

/* 1 = decodable, 0 = unsupported / fallback for JIT */
int interpreter_probe(uint32_t instr);

#endif /* POUND_INTERPRETER_H */