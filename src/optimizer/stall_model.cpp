/**
 * @file stall_model.cpp
 * @brief Static count of the pipeline bubbles an RV5S in-order pipeline inserts for a program.
 */
#include "optimizer/stall_model.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

#include "optimizer/scheduler_core.h"

namespace Kites::optimizer
{
namespace
{
/** An instruction that is still close enough behind the next one to cause a hazard. */
// Pipeline-history model: recent issued instruction data used for nearby RAW hazard checks.
struct Issued
{
    std::size_t instruction = 0;
    std::size_t slot = 0; // issue slot, counting bubbles
    bool isLoad = false;
    std::vector<std::string> writes;
};

// Control-flow helper: identify unconditional transfers that end the current linear hazard history.
bool isUnconditionalJump(const ICUnit &unit)
{
    return detail::isControlFlow(unit) && !detail::isConditionalBranch(unit);
}
} // namespace

// Stall simulation module: count bubbles in written instruction order for H_NF or H_F.
// This is a static linear scan, not path-sensitive program execution.
std::vector<StallInfo> computeStalls(const AssembledProgram &program, bool forwardingEnabled)
{
    std::vector<StallInfo> result(program.intermediate_code.size());

    std::deque<Issued> recent; // nearest instruction first
    std::size_t nextSlot = 0;

    for (std::size_t i = 0; i < program.intermediate_code.size(); ++i)
    {
        const ICUnit &unit = program.intermediate_code[i].first;
        const detail::InstructionInfo info = detail::describe(unit);

        StallInfo stall;
        // Recent instructions are traversed newest-first. Only the latest
        // writer of a register can supply the value consumed here.
        std::unordered_set<std::string> latestWriterSeen;

        for (const Issued &earlier : recent)
        {
            const std::size_t distance = nextSlot - earlier.slot;
            for (const std::string &reg : earlier.writes)
            {
                if (!latestWriterSeen.insert(reg).second)
                    continue;

                if (std::find(info.reads.begin(), info.reads.end(), reg) ==
                    info.reads.end())
                    continue;

                const std::size_t required =
                    forwardingEnabled ? (earlier.isLoad ? 2 : 1) : 3;
                if (forwardingEnabled && !earlier.isLoad)
                    continue; // forwarded from EX/MEM, no bubble
                if (distance >= required)
                    continue;

                const std::size_t needed = required - distance;
                if (needed <= stall.cycles)
                    continue;

                stall.cycles = needed;
                stall.producer = earlier.instruction;
                stall.reg = reg;
            }
        }

        nextSlot += stall.cycles;
        result[i] = stall;

        recent.push_front({i, nextSlot, info.is_load, info.writes});
        ++nextSlot;
        while (recent.size() > 2)
            recent.pop_back();

        if (isUnconditionalJump(unit))
            recent.clear();
    }
    return result;
}

// Aggregation module: sum all per-instruction bubble counts.
std::size_t totalStalls(const std::vector<StallInfo> &stalls)
{
    std::size_t total = 0;
    for (const StallInfo &stall : stalls)
        total += stall.cycles;
    return total;
}

} // namespace Kites::optimizer
