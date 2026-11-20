/**
 * @file forwarding_instruction_scheduler.cpp
 * @brief DAG-based instruction scheduling for the RV5S H_F pipeline.
 */
#include "optimizer/forwarding_instruction_scheduler.h"

#include <cstddef>
#include <vector>

#include "optimizer/scheduler_core.h"

namespace Kites::optimizer
{
namespace
{
using detail::CandidateScore;
using detail::RegionState;

/**
 * Forwarding removes ordinary ALU-to-ALU RAW stalls. A load feeding its
 * consumer, or any producer feeding a branch, still needs one slot of distance.
 */
std::size_t forwardingStalls(const RegionState &state, std::size_t candidate)
{
    const bool branchConsumer = detail::isConditionalBranch(state.instruction(candidate));
    const auto &placed = state.placed;

    std::size_t stalls = 0;
    for (std::size_t prior = 0; prior < placed.size(); ++prior)
    {
        if (!detail::hasRAWDependency(placed[prior], state.nodes[candidate]))
            continue;

        const std::size_t distance = placed.size() - prior;
        const std::size_t requiredDistance = (placed[prior].is_load) ? 2 : 1;
        if (distance < requiredDistance)
            stalls += requiredDistance - distance;
    }
    return stalls;
}

/** Prefers instructions that can fill a latency slot before a branch or load consumer. */
std::size_t latencyFill(const RegionState &state, std::size_t candidate)
{
    std::size_t fill = 0;
    for (std::size_t consumer = candidate + 1; consumer < state.count(); ++consumer)
    {
        if (state.scheduled[consumer] ||
            !detail::hasRAWDependency(state.nodes[candidate], state.nodes[consumer]))
            continue;

        if (detail::isConditionalBranch(state.instruction(consumer)) ||
            state.nodes[candidate].is_load)
            ++fill;
    }
    return fill;
}

CandidateScore scoreForwarding(const RegionState &state, std::size_t candidate)
{
    return {forwardingStalls(state, candidate), latencyFill(state, candidate)};
}

} // namespace

ScheduleResult scheduleForwardingBasicBlock(const std::vector<ICUnit> &block)
{
    detail::SchedulingPolicy policy;
    policy.score = scoreForwarding;
    return detail::scheduleBlock(block, policy);
}

} // namespace Kites::optimizer
