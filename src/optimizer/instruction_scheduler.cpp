/**
 * @file instruction_scheduler.cpp
 * @brief Conservative dependency-aware scheduling within basic blocks.
 */
#include "optimizer/instruction_scheduler.h"

#include <QDebug>
#include <QString>
#include <cstddef>
#include <sstream>
#include <utility>
#include <vector>

#include "optimizer/scheduler_core.h"

namespace Kites::optimizer
{
namespace
{
using detail::CandidateScore;
using detail::RegionState;

QString instructionText(const RegionState &state, std::size_t candidate)
{
    std::ostringstream text;
    text << state.instruction(candidate);
    return QString::fromStdString(text.str());
}

/** Without forwarding, a RAW result is usable 2 slots (adjacent) or 1 slot (gap of one) late. */
std::size_t noForwardingStalls(const RegionState &state, std::size_t candidate)
{
    const auto &placed = state.placed;
    const auto &consumer = state.nodes[candidate];

    if (!placed.empty() && detail::hasRAWDependency(placed.back(), consumer))
        return 2;
    if (placed.size() >= 2 && detail::hasRAWDependency(placed[placed.size() - 2], consumer))
        return 1;
    return 0;
}

/**
 * Producers of a branch operand should go early so independent instructions
 * sit between them and the fixed branch. Critical-path length alone cannot
 * tell them apart, since the branch depends on every earlier node.
 */
std::size_t branchOperandPriority(const RegionState &state, std::size_t candidate)
{
    std::size_t priority = 0;
    for (std::size_t consumer = candidate + 1; consumer < state.count(); ++consumer)
    {
        if (!state.scheduled[consumer] && detail::isConditionalBranch(state.instruction(consumer)) &&
            detail::hasRAWDependency(state.nodes[candidate], state.nodes[consumer]))
        {
            ++priority;
        }
    }
    return priority;
}

CandidateScore scoreNoForwarding(const RegionState &state, std::size_t candidate)
{
    return {noForwardingStalls(state, candidate), branchOperandPriority(state, candidate)};
}

void traceCandidate(const RegionState &state, std::size_t candidate)
{
    qDebug() << "[H_NF] Candidate:" << instructionText(state, candidate);
}

void traceScore(const RegionState &state, std::size_t candidate, const CandidateScore &score)
{
    qDebug() << "[H_NF] Candidate:" << instructionText(state, candidate)
             << "Stalls:" << score.stalls << "Branch priority:" << score.priority
             << "Critical path:" << state.criticalPath[candidate];
}

} // namespace

ScheduleResult scheduleBasicBlock(const std::vector<ICUnit> &basic_block)
{
    qDebug() << "[H_NF] Received block size:" << basic_block.size();

    detail::SchedulingPolicy policy;
    policy.score = scoreNoForwarding;
    policy.onCandidate = traceCandidate;
    policy.onScored = traceScore;
    return detail::scheduleBlock(basic_block, policy);
}

ScheduleResult scheduleProgramBlocks(const std::vector<ICUnit> &program,
                                     const std::vector<std::size_t> &branch_target_indices)
{
    ScheduleResult result;
    result.instructions = program;
    if (program.empty()) return result;

    std::vector<bool> leader(program.size(), false);
    leader[0] = true;
    for (std::size_t target : branch_target_indices)
        if (target < program.size()) leader[target] = true;
    for (std::size_t i = 0; i < program.size(); ++i)
        if (detail::isControlFlow(program[i]) && i + 1 < program.size()) leader[i + 1] = true;

    std::vector<ICUnit> scheduled;
    scheduled.reserve(program.size());
    std::size_t block_start = 0;
    for (std::size_t i = 1; i <= program.size(); ++i)
    {
        if (i == program.size() || leader[i])
        {
            std::vector<ICUnit> block(program.begin() + static_cast<std::ptrdiff_t>(block_start),
                                      program.begin() + static_cast<std::ptrdiff_t>(i));
            auto block_result = scheduleBasicBlock(block);
            result.moved_instructions += block_result.moved_instructions;
            scheduled.insert(scheduled.end(), block_result.instructions.begin(),
                             block_result.instructions.end());
            block_start = i;
        }
    }
    result.instructions = std::move(scheduled);
    detail::updateIndicesIfMoved(result, program);
    return result;
}

ScheduleResult scheduleLoadUseHazards(const std::vector<ICUnit> &basic_block)
{
    return scheduleBasicBlock(basic_block);
}

} // namespace Kites::optimizer
