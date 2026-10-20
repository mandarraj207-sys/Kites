/**
 * @file instruction_scheduler.cpp
 * @brief Conservative local instruction scheduling for Phase 1.
 *
 * KITES Phase 1 contribution: analyzes instruction dependencies and proposes
 * legal local reorderings. This is an extension to the existing KITES project;
 * the processor execution and hazard-detection implementation remain separate.
 */
#include "optimizer/instruction_scheduler.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Kites::optimizer
{
namespace
{
struct InstructionInfo
{
    bool supported = false;
    bool is_load = false;
    bool is_store = false;
    std::vector<std::string> reads;
    std::vector<std::string> writes;

    bool is_memory() const { return is_load || is_store; }
};

const std::unordered_set<std::string> kRTypeInteger = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhsu", "mulhu",
    "div", "divu", "rem", "remu", "mulw", "divw", "divuw", "remw", "remuw"};

const std::unordered_set<std::string> kITypeInteger = {
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "addiw", "slliw", "srliw", "sraiw"};

const std::unordered_set<std::string> kLoads = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};
const std::unordered_set<std::string> kStores = {"sb", "sh", "sw", "sd"};

bool contains(const std::unordered_set<std::string> &set, const std::string &value)
{
    return set.find(value) != set.end();
}

void addRead(std::vector<std::string> &reads, const std::string &reg)
{
    if (!reg.empty())
        reads.push_back(reg);
}

void addWrite(std::vector<std::string> &writes, const std::string &reg)
{
    // x0 always reads as zero; writes to it have no architectural effect.
    if (!reg.empty() && reg != "x0")
        writes.push_back(reg);
}

InstructionInfo describe(const ICUnit &unit)
{
    InstructionInfo info;
    const std::string op = unit.getOpcode();
    const std::string rd = unit.getRd();
    const std::string rs1 = unit.getRs1();
    const std::string rs2 = unit.getRs2();

    // A labelled instruction may be a branch target. Do not move it.
    if (!unit.getLabel().empty())
        return info;

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
        addRead(info.reads, rs1); // base register
        addWrite(info.writes, rd);
    }
    else if (contains(kStores, op))
    {
        info.supported = true;
        info.is_store = true;
        addRead(info.reads, rs1); // base register
        addRead(info.reads, rs2); // value being stored
    }
    else if (op == "lui" || op == "auipc")
    {
        info.supported = true;
        addWrite(info.writes, rd);
    }

    // Branches, jumps, floating-point/CSR instructions, pseudo-instructions,
    // fences, and unknown opcodes are deliberately unsupported in Phase 1.
    return info;
}

bool intersects(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    for (const auto &reg : a)
    {
        if (std::find(b.begin(), b.end(), reg) != b.end())
            return true;
    }
    return false;
}

bool hasDependency(const InstructionInfo &a, const InstructionInfo &b)
{
    // Preserve RAW, WAR, and WAW ordering during static reordering.
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}

bool canCross(const InstructionInfo &moving, const InstructionInfo &crossed)
{
    // Do not move an instruction across unsupported operations or memory
    // operations. Phase 1 deliberately does not perform memory-alias analysis.
    if (!moving.supported || !crossed.supported || moving.is_memory() || crossed.is_memory())
        return false;

    // hasDependency covers both directions and all register dependency types.
    return !hasDependency(moving, crossed);
}

bool readsRegister(const InstructionInfo &info, const std::string &reg)
{
    return !reg.empty() &&
           std::find(info.reads.begin(), info.reads.end(), reg) != info.reads.end();
}

bool writesRegister(const InstructionInfo &info, const std::string &reg)
{
    return !reg.empty() &&
           std::find(info.writes.begin(), info.writes.end(), reg) != info.writes.end();
}

} // namespace

ScheduleResult scheduleLoadUseHazards(const std::vector<ICUnit> &basic_block,
                                      std::size_t window_size)
{
    ScheduleResult result;
    result.instructions = basic_block;

    // At least three instructions are needed: load, consumer, candidate.
    if (basic_block.size() < 3 || window_size < 3)
        return result;

    for (std::size_t load_index = 0; load_index + 2 < result.instructions.size(); ++load_index)
    {
        const InstructionInfo load_info = describe(result.instructions[load_index]);
        if (!load_info.supported || !load_info.is_load || load_info.writes.empty())
            continue;

        const std::string loaded_register = load_info.writes.front();
        const InstructionInfo consumer_info = describe(result.instructions[load_index + 1]);
        if (!consumer_info.supported || !readsRegister(consumer_info, loaded_register))
            continue;

        // Avoid overflow in load_index + window_size for unusually large values.
        const std::size_t remaining = result.instructions.size() - load_index;
        const std::size_t span = std::min(window_size, remaining);
        const std::size_t end = load_index + span; // exclusive

        // Greedy search: find the first safe non-memory candidate in the window.
        for (std::size_t candidate_index = load_index + 2; candidate_index < end;
             ++candidate_index)
        {
            const InstructionInfo candidate_info = describe(result.instructions[candidate_index]);
            if (!candidate_info.supported || candidate_info.is_memory() ||
                readsRegister(candidate_info, loaded_register) ||
                writesRegister(candidate_info, loaded_register))
                continue;

            bool safe_to_move = true;
            // Moving the candidate to load_index + 1 crosses every instruction
            // between its current position and the load's immediate consumer.
            for (std::size_t crossed_index = candidate_index;
                 crossed_index > load_index + 1; --crossed_index)
            {
                const InstructionInfo crossed_info = describe(result.instructions[crossed_index - 1]);
                if (!canCross(candidate_info, crossed_info))
                {
                    safe_to_move = false;
                    break;
                }
            }

            if (!safe_to_move)
                continue;

            ICUnit candidate = result.instructions[candidate_index];
            result.instructions.erase(result.instructions.begin() +
                                      static_cast<std::ptrdiff_t>(candidate_index));
            result.instructions.insert(result.instructions.begin() +
                                            static_cast<std::ptrdiff_t>(load_index + 1),
                                        candidate);
            ++result.moved_instructions;
            break; // One move fills this load's immediate gap.
        }
    }

    // Only update indices if an instruction actually moved. The input remains
    // untouched, and original source line numbers/labels are preserved.
    if (result.moved_instructions > 0 && !result.instructions.empty())
    {
        const unsigned int first_index = basic_block.front().getInstructionIndex();
        for (std::size_t i = 0; i < result.instructions.size(); ++i)
        {
            result.instructions[i].setInstructionIndex(
                first_index + static_cast<unsigned int>(i));
        }
    }

    return result;
}

} // namespace Kites::optimizer
