#include "optimizer/instruction_scheduler.h"
#include "optimizer/scheduler_config.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using Kites::ICUnit;
using Kites::optimizer::scheduleLoadUseHazards;

static ICUnit makeInstruction(unsigned int index, const std::string &opcode,
                              const std::string &rd, const std::string &rs1,
                              const std::string &rs2 = "")
{
    ICUnit unit;
    unit.setInstructionIndex(index);
    unit.setOpcode(opcode);
    unit.setRd(rd);
    unit.setRs1(rs1);
    unit.setRs2(rs2);
    return unit;
}

int main()
{
    // Independent addi should be moved between a load and its consumer.
    std::vector<ICUnit> block = {
        makeInstruction(10, "ld", "x1", "x2"),
        makeInstruction(11, "add", "x3", "x1", "x4"),
        makeInstruction(12, "addi", "x5", "x6")};

    auto result = scheduleLoadUseHazards(block, 3);
    assert(result.moved_instructions == 1);
    assert(result.instructions[0].getOpcode() == "ld");
    assert(result.instructions[1].getOpcode() == "addi");
    assert(result.instructions[2].getOpcode() == "add");
    assert(result.instructions[0].getInstructionIndex() == 10);
    assert(result.instructions[1].getInstructionIndex() == 11);
    assert(result.instructions[2].getInstructionIndex() == 12);

    // Candidate writes the consumer's source register: moving it would change semantics.
    std::vector<ICUnit> war_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "addi", "x1", "x6")};
    auto war_result = scheduleLoadUseHazards(war_block, 3);
    assert(war_result.moved_instructions == 0);

    // Candidate consumes the loaded register itself: it cannot fill the delay slot.
    std::vector<ICUnit> raw_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x1")};
    auto raw_result = scheduleLoadUseHazards(raw_block, 3);
    assert(raw_result.moved_instructions == 0);

    // Memory operations are conservatively treated as barriers.
    std::vector<ICUnit> memory_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "lw", "x5", "x6")};
    auto memory_result = scheduleLoadUseHazards(memory_block, 3);
    assert(memory_result.moved_instructions == 0);

    // Window boundary: 2 cannot include a candidate; 3 includes exactly one.
    auto small_window_result = scheduleLoadUseHazards(block, 2);
    assert(small_window_result.moved_instructions == 0);

    auto boundary_window_result = scheduleLoadUseHazards(block, 3);
    assert(boundary_window_result.moved_instructions == 1);
    assert(boundary_window_result.instructions[1].getOpcode() == "addi");

    // The default argument reads the centralized global configuration.
    const std::size_t old_window = Kites::optimizer::g_scheduler_window_size;
    Kites::optimizer::g_scheduler_window_size = 3;
    auto configured_result = scheduleLoadUseHazards(block);
    assert(configured_result.moved_instructions == 1);
    Kites::optimizer::g_scheduler_window_size = old_window;

    // The scheduler returns a reordered copy and must not mutate its input.
    assert(block[1].getOpcode() == "add");
    assert(block[2].getOpcode() == "addi");

    std::cout << "All instruction scheduler tests passed.\n";
    return 0;
}
