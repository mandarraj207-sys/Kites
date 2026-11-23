/**
 * @file scheduler_core.h
 * @brief Machinery shared by the H_NF and H_F instruction schedulers.
 */
#ifndef KITES_SCHEDULER_CORE_H
#define KITES_SCHEDULER_CORE_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "optimizer/instruction_scheduler.h"

namespace Kites::optimizer::detail
{
// Instruction metadata model: supported status, load/store flags, and register read/write sets.
struct InstructionInfo
{
    bool supported = false;
    bool is_load = false;
    bool is_store = false;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

InstructionInfo describe(const ICUnit &unit);

bool isConditionalBranch(const ICUnit &unit);
bool isControlFlow(const ICUnit &unit);

/** RAW, WAR or WAW: the two instructions must keep their relative order. */
bool hasDependency(const InstructionInfo &a, const InstructionInfo &b);

/** Only RAW imposes a result-availability delay. */
bool hasRAWDependency(const InstructionInfo &producer, const InstructionInfo &consumer);

/** Read-only view of a scheduling region while it is being ordered. */
// Scheduler state view: block, active region, dependency metadata, and already placed instructions.
struct RegionState
{
    const std::vector<ICUnit> &block;
    std::size_t start;
    const std::vector<InstructionInfo> &nodes;
    const std::vector<std::size_t> &criticalPath;
    const std::vector<bool> &scheduled;
    const std::vector<InstructionInfo> &placed;

    const ICUnit &instruction(std::size_t node) const { return block[start + node]; }
    std::size_t count() const { return nodes.size(); }
};

/** Lower stalls win, then higher priority, then longer critical path. */
// Candidate score model: stall cost and policy-specific priority.
struct CandidateScore
{
    std::size_t stalls = 0;
    std::size_t priority = 0;
};

// Policy interface: H_NF/H_F scoring callbacks plus optional debug tracing hooks.
struct SchedulingPolicy
{
    std::function<CandidateScore(const RegionState &, std::size_t)> score;

    // Optional tracing hooks.
    std::function<void(const RegionState &, std::size_t)> onCandidate;
    std::function<void(const RegionState &, std::size_t, const CandidateScore &)> onScored;
};

/** Splits the block into regions and reorders each one using the policy. */
// API: shared scheduling engine used by H_NF and H_F.
ScheduleResult scheduleBlock(const std::vector<ICUnit> &block, const SchedulingPolicy &policy);

// API: update instruction indices after reordering.
void updateIndicesIfMoved(ScheduleResult &result, const std::vector<ICUnit> &original);

} // namespace Kites::optimizer::detail

#endif // KITES_SCHEDULER_CORE_H
