/**
 * @file instruction_scheduler.cpp
 * @brief Conservative dependency-aware scheduling within basic blocks.
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
const std::unordered_set<std::string> kControlFlow = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "jal", "jalr", "c.j", "c.jr",
    "ecall", "ebreak", "mret", "sret"};

bool contains(const std::unordered_set<std::string> &set, const std::string &value)
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

InstructionInfo describe(const ICUnit &unit)
{
    InstructionInfo info;
    const std::string op = unit.getOpcode();
    const std::string rd = unit.getRd();
    const std::string rs1 = unit.getRs1();
    const std::string rs2 = unit.getRs2();

    // Labels on ICUnits represent control-flow targets/references in the
    // assembler's intermediate form. Keep such instructions immovable.
    if (!unit.getLabel().empty() || contains(kControlFlow, op)) return info;

    if (contains(kRTypeInteger, op))
    {
        info.supported = true;
        addRead(info.reads, rs1); addRead(info.reads, rs2); addWrite(info.writes, rd);
    }
    else if (contains(kITypeInteger, op))
    {
        info.supported = true;
        addRead(info.reads, rs1); addWrite(info.writes, rd);
    }
    else if (contains(kLoads, op))
    {
        info.supported = true; info.is_load = true;
        addRead(info.reads, rs1); addWrite(info.writes, rd);
    }
    else if (contains(kStores, op))
    {
        info.supported = true; info.is_store = true;
        addRead(info.reads, rs1); addRead(info.reads, rs2);
    }
    else if (op == "lui" || op == "auipc")
    {
        info.supported = true; addWrite(info.writes, rd);
    }
    return info;
}

bool intersects(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    for (const auto &reg : a)
        if (std::find(b.begin(), b.end(), reg) != b.end()) return true;
    return false;
}

bool hasDependency(const InstructionInfo &a, const InstructionInfo &b)
{
    // RAW, WAR, and WAW dependencies must retain their relative order.
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}

bool canCross(const InstructionInfo &moving, const InstructionInfo &crossed)
{
    // Memory alias analysis is intentionally out of scope: memory ops are barriers.
    if (!moving.supported || !crossed.supported || moving.is_memory() || crossed.is_memory())
        return false;
    return !hasDependency(moving, crossed);
}

bool hasRAWDependency(const InstructionInfo &producer, const InstructionInfo &consumer)
{
    // Only read-after-write dependencies impose a result-availability delay.
    return intersects(producer.writes, consumer.reads);
}

bool isControlFlow(const ICUnit &unit)
{
    return contains(kControlFlow, unit.getOpcode());
}

void updateIndicesIfMoved(ScheduleResult &result, const std::vector<ICUnit> &original)
{
    if (result.moved_instructions == 0 || original.empty()) return;
    const unsigned int first_index = original.front().getInstructionIndex();
    for (std::size_t i = 0; i < result.instructions.size(); ++i)
        result.instructions[i].setInstructionIndex(first_index + static_cast<unsigned int>(i));
}

} // namespace

ScheduleResult scheduleBasicBlock(const std::vector<ICUnit> &basic_block,
                                  std::size_t window_size)
{
    ScheduleResult result;
    result.instructions = basic_block;
    if (basic_block.size() < 3 || window_size < 3) return result;

    // Greedy local list scheduling: when adjacent instructions have a register
    // dependency, try to move a later independent instruction between them.
    // This covers load-use and ordinary ALU producer-consumer hazards.
    for (std::size_t producer_index = 0; producer_index + 2 < result.instructions.size();
         ++producer_index)
    {
        const InstructionInfo producer = describe(result.instructions[producer_index]);
        const InstructionInfo consumer = describe(result.instructions[producer_index + 1]);
        if (!producer.supported || !consumer.supported || producer.writes.empty() ||
            !hasRAWDependency(producer, consumer))
            continue;

        const std::size_t remaining = result.instructions.size() - producer_index;
        const std::size_t end = producer_index + std::min(window_size, remaining);
        for (std::size_t candidate_index = producer_index + 2; candidate_index < end;
             ++candidate_index)
        {
            const InstructionInfo candidate = describe(result.instructions[candidate_index]);
            if (!candidate.supported || candidate.is_memory() ||
                hasDependency(candidate, producer) || hasDependency(candidate, consumer))
                continue;

            bool safe = true;
            for (std::size_t crossed_index = producer_index + 1;
                 crossed_index < candidate_index; ++crossed_index)
            {
                if (!canCross(candidate, describe(result.instructions[crossed_index])))
                {
                    safe = false;
                    break;
                }
            }
            if (!safe) continue;

            ICUnit moving = result.instructions[candidate_index];
            result.instructions.erase(result.instructions.begin() +
                                      static_cast<std::ptrdiff_t>(candidate_index));
            result.instructions.insert(result.instructions.begin() +
                                            static_cast<std::ptrdiff_t>(producer_index + 1),
                                        moving);
            ++result.moved_instructions;
            // Re-evaluate this producer against its new adjacent instruction.
            if (producer_index > 0) --producer_index;
            break;
        }
    }

    updateIndicesIfMoved(result, basic_block);
    return result;
}

ScheduleResult scheduleProgramBlocks(const std::vector<ICUnit> &program,
                                     const std::vector<std::size_t> &branch_target_indices,
                                     std::size_t window_size)
{
    ScheduleResult result;
    result.instructions = program;
    if (program.empty()) return result;

    std::vector<bool> leader(program.size(), false);
    leader[0] = true;
    for (std::size_t target : branch_target_indices)
        if (target < program.size()) leader[target] = true;
    for (std::size_t i = 0; i < program.size(); ++i)
        if (isControlFlow(program[i]) && i + 1 < program.size()) leader[i + 1] = true;

    std::vector<ICUnit> scheduled;
    scheduled.reserve(program.size());
    std::size_t block_start = 0;
    for (std::size_t i = 1; i <= program.size(); ++i)
    {
        if (i == program.size() || leader[i])
        {
            std::vector<ICUnit> block(program.begin() + static_cast<std::ptrdiff_t>(block_start),
                                      program.begin() + static_cast<std::ptrdiff_t>(i));
            auto block_result = scheduleBasicBlock(block, window_size);
            result.moved_instructions += block_result.moved_instructions;
            scheduled.insert(scheduled.end(), block_result.instructions.begin(),
                             block_result.instructions.end());
            block_start = i;
        }
    }
    result.instructions = std::move(scheduled);
    updateIndicesIfMoved(result, program);
    return result;
}

ScheduleResult scheduleLoadUseHazards(
    const std::vector<ICUnit> &basic_block,
    std::size_t window_size)
{
    return scheduleBasicBlock(basic_block, window_size);
}

/*
ScheduleResult scheduleLoadUseHazards(const std::vector<ICUnit> &basic_block,
                                      std::size_t window_size)
{
    // Keep the Phase 1 API focused on load-use hazards. General RAW scheduling
    // remains available through scheduleBasicBlock().
    ScheduleResult result;
    result.instructions = basic_block;
    if (basic_block.size() < 3 || window_size < 3)
        return result;

    for (std::size_t load_index = 0;
         load_index + 2 < result.instructions.size(); ++load_index)
    {
        const InstructionInfo load_info = describe(result.instructions[load_index]);
        if (!load_info.supported || !load_info.is_load || load_info.writes.empty())
            continue;

        const std::string loaded_register = load_info.writes.front();
        const InstructionInfo consumer_info = describe(result.instructions[load_index + 1]);
        if (!consumer_info.supported ||
            !intersects(load_info.writes, consumer_info.reads))
            continue;

        const std::size_t remaining = result.instructions.size() - load_index;
        const std::size_t end = load_index + std::min(window_size, remaining);
        for (std::size_t candidate_index = load_index + 2;
             candidate_index < end; ++candidate_index)
        {
            const InstructionInfo candidate_info = describe(result.instructions[candidate_index]);
            if (!candidate_info.supported || candidate_info.is_memory() ||
                std::find(candidate_info.reads.begin(), candidate_info.reads.end(), loaded_register)
                    != candidate_info.reads.end() ||
                std::find(candidate_info.writes.begin(), candidate_info.writes.end(), loaded_register)
                    != candidate_info.writes.end())
                continue;

            bool safe = true;
            for (std::size_t crossed_index = load_index + 1;
                 crossed_index < candidate_index; ++crossed_index)
            {
                if (!canCross(candidate_info, describe(result.instructions[crossed_index])))
                {
                    safe = false;
                    break;
                }
            }
            if (!safe)
                continue;

            ICUnit moving = result.instructions[candidate_index];
            result.instructions.erase(result.instructions.begin() +
                                      static_cast<std::ptrdiff_t>(candidate_index));
            result.instructions.insert(result.instructions.begin() +
                                           static_cast<std::ptrdiff_t>(load_index + 1),
                                       moving);
            ++result.moved_instructions;
            break; // one filler per load-use dependency
        }
    }

    updateIndicesIfMoved(result, basic_block);
    return result;
}
*/

} // namespace Kites::optimizer
