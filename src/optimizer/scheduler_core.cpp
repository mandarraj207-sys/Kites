/**
 * @file scheduler_core.cpp
 * @brief Machinery shared by the H_NF and H_F instruction schedulers.
 */
#include "optimizer/scheduler_core.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace Kites::optimizer::detail
{
namespace
{
using OpcodeSet = std::unordered_set<std::string>;
using DependencyGraph = std::vector<std::vector<bool>>;

const OpcodeSet kRTypeInteger = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu", "mulhu",
    "div", "divu", "rem", "remu", "mulw", "divw", "divuw", "remw", "remuw"};
const OpcodeSet kITypeInteger = {
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "addiw", "slliw", "srliw", "sraiw"};
const OpcodeSet kLoads = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};
const OpcodeSet kStores = {"sb", "sh", "sw", "sd"};
const OpcodeSet kConditionalBranches = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "c.beqz", "c.bnez"};
const OpcodeSet kControlFlow = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "c.beqz", "c.bnez",
    "jal", "jalr", "j", "jr", "c.j", "c.jal", "c.jr", "c.jalr",
    "ecall", "ebreak", "c.ebreak", "mret", "sret", "uret", "dret"};

bool contains(const OpcodeSet &set, const std::string &value)
{
    return set.find(value) != set.end();
}

void addRead(std::vector<std::string> &reads, const std::string &reg)
{
    if (!reg.empty()) reads.push_back(reg);
}

void addWrite(std::vector<std::string> &writes, const std::string &reg)
{
    if (!reg.empty() && reg != "x0") writes.push_back(reg);
}

bool intersects(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    for (const auto &reg : a)
        if (std::find(b.begin(), b.end(), reg) != b.end()) return true;
    return false;
}

bool isSchedulable(const InstructionInfo &info)
{
    return info.supported && !info.is_store;
}

bool isAuipc(const ICUnit &unit)
{
    return unit.getOpcode() == "auipc";
}

std::vector<InstructionInfo> describeAll(const std::vector<ICUnit> &block)
{
    std::vector<InstructionInfo> infos;
    infos.reserve(block.size());
    for (const auto &unit : block)
        infos.push_back(describe(unit));
    return infos;
}

/**
 * Returns the end of the schedulable run starting at `start`. A conditional
 * branch is included as the fixed final instruction of its region.
 */
std::size_t findRegionEnd(const std::vector<ICUnit> &block,
                          const std::vector<InstructionInfo> &infos, std::size_t start)
{
    std::size_t end = start;
    while (end < block.size())
    {
        if (!isSchedulable(infos[end]))
            break;

        ++end;
        if (isConditionalBranch(block[end - 1]))
            break;
    }
    return end;
}

/**
 * Edges point forward in the original order and preserve every register
 * dependency, the order of loads (no alias analysis), and the terminal branch.
 */
DependencyGraph buildDependencyGraph(const std::vector<ICUnit> &block, std::size_t start,
                                     const std::vector<InstructionInfo> &nodes)
{
    const std::size_t count = nodes.size();
    DependencyGraph edges(count, std::vector<bool>(count, false));

    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::size_t j = i + 1; j < count; ++j)
        {
            const bool loadOrder = nodes[i].is_load && nodes[j].is_load;
            const bool terminalBranch = j == count - 1 && isConditionalBranch(block[start + j]);
            edges[i][j] = hasDependency(nodes[i], nodes[j]) || loadOrder || terminalBranch;
        }
    }
    return edges;
}

std::vector<std::size_t> computeCriticalPaths(const DependencyGraph &edges)
{
    const std::size_t count = edges.size();
    std::vector<std::size_t> criticalPath(count, 1);

    for (std::size_t i = count; i-- > 0;)
    {
        for (std::size_t j = i + 1; j < count; ++j)
        {
            if (edges[i][j])
                criticalPath[i] = std::max(criticalPath[i], criticalPath[j] + 1);
        }
    }
    return criticalPath;
}

/**
 * AUIPC is PC-relative: it stays in its original output slot, and nothing
 * else may take that slot or be replaced by it.
 */
bool violatesAuipcPin(const RegionState &state, std::size_t outputSlot, std::size_t candidate)
{
    const bool slotIsAuipc = isAuipc(state.instruction(outputSlot));
    const bool candidateIsAuipc = isAuipc(state.instruction(candidate));
    return (slotIsAuipc && candidate != outputSlot) || (!slotIsAuipc && candidateIsAuipc);
}

bool isReady(const DependencyGraph &edges, const std::vector<bool> &scheduled,
             std::size_t candidate)
{
    for (std::size_t pred = 0; pred < candidate; ++pred)
        if (edges[pred][candidate] && !scheduled[pred]) return false;
    return true;
}

bool isBetter(const CandidateScore &score, std::size_t path, const CandidateScore &best,
              std::size_t bestPath)
{
    if (score.stalls != best.stalls) return score.stalls < best.stalls;
    if (score.priority != best.priority) return score.priority > best.priority;
    return path > bestPath;
}

/** Returns the new region order, or a shorter vector if the graph was inconsistent. */
std::vector<std::size_t> orderRegion(const std::vector<ICUnit> &block, std::size_t start,
                                     const std::vector<InstructionInfo> &nodes,
                                     const SchedulingPolicy &policy)
{
    const std::size_t count = nodes.size();
    const DependencyGraph edges = buildDependencyGraph(block, start, nodes);
    const std::vector<std::size_t> criticalPath = computeCriticalPaths(edges);

    std::vector<bool> scheduled(count, false);
    std::vector<std::size_t> order;
    std::vector<InstructionInfo> placed;
    order.reserve(count);
    placed.reserve(count);

    const RegionState state{block, start, nodes, criticalPath, scheduled, placed};

    while (order.size() < count)
    {
        std::size_t best = count;
        CandidateScore bestScore;
        std::size_t bestPath = 0;

        for (std::size_t candidate = 0; candidate < count; ++candidate)
        {
            if (scheduled[candidate])
                continue;

            if (policy.onCandidate)
                policy.onCandidate(state, candidate);

            if (violatesAuipcPin(state, order.size(), candidate))
                continue;

            if (!isReady(edges, scheduled, candidate))
                continue;

            const CandidateScore score = policy.score(state, candidate);

            if (policy.onScored)
                policy.onScored(state, candidate, score);

            if (best == count || isBetter(score, criticalPath[candidate], bestScore, bestPath))
            {
                best = candidate;
                bestScore = score;
                bestPath = criticalPath[candidate];
            }
        }

        // Guard against bugs: the graph is acyclic, so a candidate always exists.
        if (best == count)
            break;

        scheduled[best] = true;
        order.push_back(best);
        placed.push_back(nodes[best]);
    }
    return order;
}

void applyOrder(ScheduleResult &result, const std::vector<ICUnit> &block, std::size_t start,
                const std::vector<std::size_t> &order)
{
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        result.instructions[start + i] = block[start + order[i]];

        if (order[i] != i)
            ++result.moved_instructions;
    }
}

} // namespace

InstructionInfo describe(const ICUnit &unit)
{
    InstructionInfo info;
    const std::string op = unit.getOpcode();
    const std::string rd = unit.getRd();
    const std::string rs1 = unit.getRs1();
    const std::string rs2 = unit.getRs2();

    // For a branch, getLabel() is its TARGET label, not a label attached to
    // this instruction, so branches are recognized before the label barrier.
    if (contains(kConditionalBranches, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addRead(info.reads, rs2);
        return info;
    }

    if (!unit.getLabel().empty()) return info;
    if (contains(kControlFlow, op)) return info;

    if (contains(kRTypeInteger, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addRead(info.reads, rs2);
        addWrite(info.writes, rd);
    }
    else if (contains(kITypeInteger, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addWrite(info.writes, rd);
    }
    else if (contains(kLoads, op))
    {
        info.supported = true;
        info.is_load = true;
        addRead(info.reads, rs1);
        addWrite(info.writes, rd);
    }
    else if (contains(kStores, op))
    {
        info.supported = true;
        info.is_store = true;
        addRead(info.reads, rs1);
        addRead(info.reads, rs2);
    }
    else if (op == "lui" || op == "auipc")
    {
        info.supported = true;
        addWrite(info.writes, rd);
    }
    return info;
}

bool isConditionalBranch(const ICUnit &unit)
{
    return contains(kConditionalBranches, unit.getOpcode());
}

bool isControlFlow(const ICUnit &unit)
{
    return contains(kControlFlow, unit.getOpcode());
}

bool hasDependency(const InstructionInfo &a, const InstructionInfo &b)
{
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}

bool hasRAWDependency(const InstructionInfo &producer, const InstructionInfo &consumer)
{
    return intersects(producer.writes, consumer.reads);
}

ScheduleResult scheduleBlock(const std::vector<ICUnit> &block, const SchedulingPolicy &policy)
{
    ScheduleResult result;
    result.instructions = block;

    const std::size_t n = block.size();
    if (n < 2)
        return result;

    const std::vector<InstructionInfo> infos = describeAll(block);
    std::size_t regionStart = 0;

    while (regionStart < n)
    {
        // Stores and unsupported instructions are barriers; loads may move.
        if (!isSchedulable(infos[regionStart]))
        {
            ++regionStart;
            continue;
        }

        const std::size_t regionEnd = findRegionEnd(block, infos, regionStart);
        const std::size_t count = regionEnd - regionStart;

        if (count >= 2)
        {
            const std::vector<InstructionInfo> nodes(infos.begin() + regionStart,
                                                     infos.begin() + regionEnd);
            const auto order = orderRegion(block, regionStart, nodes, policy);

            // Reorder only if the scheduler produced a complete valid ordering.
            if (order.size() == count)
                applyOrder(result, block, regionStart, order);
        }

        regionStart = regionEnd;
    }

    updateIndicesIfMoved(result, block);
    return result;
}

void updateIndicesIfMoved(ScheduleResult &result, const std::vector<ICUnit> &original)
{
    if (result.moved_instructions == 0 || original.empty()) return;

    const unsigned int firstIndex = original.front().getInstructionIndex();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
        result.instructions[i].setInstructionIndex(firstIndex + static_cast<unsigned int>(i));
}

} // namespace Kites::optimizer::detail
