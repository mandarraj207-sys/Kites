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
// Instruction classification module: opcode groups used to decode register access and control-flow behavior.
using OpcodeSet = std::unordered_set<std::string>;
using DependencyGraph = std::vector<std::vector<bool>>;

const OpcodeSet kRTypeInteger = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu", "mulhu",
    "div", "divu", "rem", "remu", "mulw", "divw", "divuw", "remw", "remuw"};
const OpcodeSet kITypeInteger = {
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "addiw", "slliw", "srliw", "sraiw"};
// Floating-point instructions supported by the assembler. Register names are kept
// as-is ("fN" versus "xN"), so integer and FP register dependencies stay distinct.
const OpcodeSet kFPBinary = {
    "fadd.s", "fsub.s", "fmul.s", "fdiv.s", "fadd.d", "fsub.d", "fmul.d", "fdiv.d",
    "fsgnj.s", "fsgnjn.s", "fsgnjx.s", "fmin.s", "fmax.s",
    "fsgnj.d", "fsgnjn.d", "fsgnjx.d", "fmin.d", "fmax.d"};
const OpcodeSet kFPCompare = {"feq.s", "flt.s", "fle.s", "feq.d", "flt.d", "fle.d"};
const OpcodeSet kFPUnary = {"fsqrt.s", "fsqrt.d"};
const OpcodeSet kFPFused = {
    "fmadd.s", "fmsub.s", "fnmsub.s", "fnmadd.s",
    "fmadd.d", "fmsub.d", "fnmsub.d", "fnmadd.d"};
const OpcodeSet kFPLoads = {"flw", "fld"};
const OpcodeSet kFPStores = {"fsw", "fsd"};
const OpcodeSet kFPClass = {"fclass.s", "fclass.d"};
const OpcodeSet kFPMoveToGPR = {"fmv.x.w", "fmv.x.d"};
const OpcodeSet kFPMoveToFPR = {"fmv.w.x", "fmv.d.x"};
const OpcodeSet kFPCvtToGPR = {
    "fcvt.w.s", "fcvt.wu.s", "fcvt.l.s", "fcvt.lu.s",
    "fcvt.w.d", "fcvt.wu.d", "fcvt.l.d", "fcvt.lu.d"};
const OpcodeSet kFPCvtToFPR = {
    "fcvt.s.w", "fcvt.s.wu", "fcvt.s.l", "fcvt.s.lu",
    "fcvt.d.w", "fcvt.d.wu", "fcvt.d.l", "fcvt.d.lu",
    "fcvt.s.d", "fcvt.d.s"};
const OpcodeSet kLoads = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};
const OpcodeSet kStores = {"sb", "sh", "sw", "sd"};
const OpcodeSet kConditionalBranches = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "c.beqz", "c.bnez"};
const OpcodeSet kControlFlow = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "c.beqz", "c.bnez",
    "jal", "jalr", "j", "jr", "c.j", "c.jal", "c.jr", "c.jalr",
    "ecall", "ebreak", "c.ebreak", "mret", "sret", "uret", "dret"};

// Opcode helper: test membership in a supported opcode group.
bool contains(const OpcodeSet &set, const std::string &value)
{
    return set.find(value) != set.end();
}

// Register metadata helper: record a non-empty source register.
void addRead(std::vector<std::string> &reads, const std::string &reg)
{
    if (!reg.empty()) reads.push_back(reg);
}

// Register metadata helper: record a non-empty destination register, excluding x0.
void addWrite(std::vector<std::string> &writes, const std::string &reg)
{
    if (!reg.empty() && reg != "x0") writes.push_back(reg);
}

// Dependency helper: test whether two register sets overlap.
bool intersects(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    for (const auto &reg : a)
        if (std::find(b.begin(), b.end(), reg) != b.end()) return true;
    return false;
}

// Barrier helper: only supported non-store instructions are eligible for reordering.
bool isSchedulable(const InstructionInfo &info)
{
    return info.supported && !info.is_store;
}

bool isAuipc(const ICUnit &unit)
{
    return unit.getOpcode() == "auipc";
}

// Decoding module: generate register-access metadata for every instruction in a block.
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
// Region detection module: find a schedulable run; a conditional branch is included only as its fixed final node.
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
// Dependency graph module: preserve RAW/WAR/WAW hazards, load order, and the terminal branch.
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

// Critical-path module: calculate longest downstream dependency-chain lengths.
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
// PC-relative safety module: keep AUIPC in its original output slot.
bool violatesAuipcPin(const RegionState &state, std::size_t outputSlot, std::size_t candidate)
{
    const bool slotIsAuipc = isAuipc(state.instruction(outputSlot));
    const bool candidateIsAuipc = isAuipc(state.instruction(candidate));
    return (slotIsAuipc && candidate != outputSlot) || (!slotIsAuipc && candidateIsAuipc);
}

// Readiness is maintained incrementally using remainingPredecessors.

// Candidate selection module: apply the existing score, priority, and critical-path tie-breaking.
bool isBetter(const CandidateScore &score, std::size_t path, const CandidateScore &best,
              std::size_t bestPath)
{
    if (score.stalls != best.stalls) return score.stalls < best.stalls;
    if (score.priority != best.priority) return score.priority > best.priority;
    return path > bestPath;
}

/** Returns the new region order, or a shorter vector if the graph was inconsistent. */
// Core scheduling module: select ready instructions and update predecessor counts incrementally.
std::vector<std::size_t> orderRegion(const std::vector<ICUnit> &block, std::size_t start,
                                     const std::vector<InstructionInfo> &nodes,
                                     const SchedulingPolicy &policy)
{
    const std::size_t count = nodes.size();
    const DependencyGraph edges = buildDependencyGraph(block, start, nodes);
    const std::vector<std::size_t> criticalPath = computeCriticalPaths(edges);

    // Count unscheduled predecessors for every instruction.
    std::vector<std::size_t> remainingPredecessors(count, 0);

    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::size_t j = i + 1; j < count; ++j)
        {
            if (edges[i][j])
                ++remainingPredecessors[j];
        }
    }

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

            if (remainingPredecessors[candidate] != 0)
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

        // Scheduling this instruction makes its successors one dependency
        // closer to becoming ready.
        for (std::size_t successor = best + 1; successor < count; ++successor)
        {
            if (edges[best][successor])
                --remainingPredecessors[successor];
        }
    }
    return order;
}

// Reordering module: apply the selected node order and count moved instructions.
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

// Instruction decoder module: classify an instruction and record its register reads/writes.
InstructionInfo describe(const ICUnit &unit)
{
    InstructionInfo info;
    const std::string op = unit.getOpcode();
    const std::string rd = unit.getRd();
    const std::string rs1 = unit.getRs1();
    const std::string rs2 = unit.getRs2();
    const std::string rs3 = unit.getRs3();

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
    else if (contains(kFPLoads, op))
    {
        info.supported = true;
        info.is_load = true;
        addRead(info.reads, rs1); // integer base address
        addWrite(info.writes, rd); // floating-point destination
    }
    else if (contains(kFPStores, op))
    {
        info.supported = true;
        info.is_store = true;
        addRead(info.reads, rs1); // integer base address
        addRead(info.reads, rd);  // floating-point value being stored
    }
    else if (contains(kFPFused, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addRead(info.reads, rs2);
        addRead(info.reads, rs3);
        addWrite(info.writes, rd);
    }
    else if (contains(kFPBinary, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addRead(info.reads, rs2);
        addWrite(info.writes, rd);
    }
    else if (contains(kFPUnary, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addWrite(info.writes, rd);
    }
    else if (contains(kFPCompare, op))
    {
        // FP comparisons consume TWO floating-point operands and produce
        // an integer-register result. Omitting rs2 can let the scheduler
        // move a comparison before the instruction that produces rs2.
        info.supported = true;
        addRead(info.reads, rs1);
        addRead(info.reads, rs2);
        addWrite(info.writes, rd);
    }
    else if (contains(kFPClass, op) || contains(kFPMoveToGPR, op) ||
             contains(kFPCvtToGPR, op))
    {
        info.supported = true;
        addRead(info.reads, rs1);
        addWrite(info.writes, rd); // unary FP-to-GPR operation
    }
    else if (contains(kFPMoveToFPR, op))
    {
        info.supported = true;
        addRead(info.reads, rs1); // integer source
        addWrite(info.writes, rd); // floating-point destination
    }
    else if (contains(kFPCvtToFPR, op))
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

// Control-flow helper: identify conditional branch instructions.
bool isConditionalBranch(const ICUnit &unit)
{
    return contains(kConditionalBranches, unit.getOpcode());
}

// Control-flow helper: identify branches, jumps, traps, and returns.
bool isControlFlow(const ICUnit &unit)
{
    return contains(kControlFlow, unit.getOpcode());
}

// Dependency helper: detect RAW, WAR, or WAW hazards.
bool hasDependency(const InstructionInfo &a, const InstructionInfo &b)
{
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}

// Timing helper: detect RAW dependencies that impose result-availability delays.
bool hasRAWDependency(const InstructionInfo &producer, const InstructionInfo &consumer)
{
    return intersects(producer.writes, consumer.reads);
}

// Shared scheduling module: split at barriers and schedule each valid region with the supplied H_NF/H_F policy.
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

// Metadata helper: refresh instruction indices after a reorder.
void updateIndicesIfMoved(ScheduleResult &result, const std::vector<ICUnit> &original)
{
    if (result.moved_instructions == 0 || original.empty()) return;

    const unsigned int firstIndex = original.front().getInstructionIndex();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
        result.instructions[i].setInstructionIndex(firstIndex + static_cast<unsigned int>(i));
}

} // namespace Kites::optimizer::detail
