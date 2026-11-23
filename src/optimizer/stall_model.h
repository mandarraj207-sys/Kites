/**
 * @file stall_model.h
 * @brief Static count of the pipeline bubbles an RV5S in-order pipeline inserts for a program.
 */
#ifndef KITES_STALL_MODEL_H
#define KITES_STALL_MODEL_H

#include <cstddef>
#include <string>
#include <vector>

#include "common/assembled_program.h"

namespace Kites::optimizer
{
/** Bubbles the pipeline inserts *before* one instruction, and the hazard behind them. */
// Result model: bubble count before an instruction and the producer/register responsible.
struct StallInfo
{
    std::size_t cycles = 0;   ///< number of bubble cycles in front of the instruction
    std::size_t producer = 0; ///< index of the instruction whose result it waits for
    std::string reg;          ///< register that producer writes
};

/**
 * One entry per instruction, mirroring the hazard detection unit (rv5s_hdu.cpp):
 *  - no forwarding: a RAW result is usable 3 issue slots after its producer, so a
 *    dependent instruction directly behind it waits 2 cycles and one with a single
 *    instruction between them waits 1 cycle;
 *  - forwarding: only a load followed directly by its consumer stalls (1 cycle).
 * Bubbles already inserted count as distance, exactly as in the real pipeline.
 *
 * The program is read as written: execution is assumed to fall through every
 * conditional branch and label, and the chain is cut after an unconditional jump.
 * This is not the same as the benchmark, which counts only the path that actually runs.
 */
// API: calculate per-instruction pipeline stalls for the selected forwarding mode.
std::vector<StallInfo> computeStalls(const AssembledProgram &program, bool forwardingEnabled);

/** Sum of `cycles` over all instructions. */
// API: sum bubble counts returned by computeStalls.
std::size_t totalStalls(const std::vector<StallInfo> &stalls);

} // namespace Kites::optimizer

#endif // KITES_STALL_MODEL_H
