#include "optimizer/instruction_scheduler.h"
#include "optimizer/scheduler_config.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using Kites::ICUnit;
using Kites::optimizer::scheduleBasicBlock;
using Kites::optimizer::scheduleLoadUseHazards;
using Kites::optimizer::scheduleProgramBlocks;

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

TEST(InstructionSchedulerTest, PreservesPhaseOneLoadUseBehavior)
{
    // Phase 1 behavior: independent instruction fills a load-use gap.
    std::vector<ICUnit> load_block = {
        makeInstruction(10, "ld", "x1", "x2"),
        makeInstruction(11, "add", "x3", "x1", "x4"),
        makeInstruction(12, "addi", "x5", "x6")};
    auto load_result = scheduleLoadUseHazards(load_block, 3);
    EXPECT_TRUE(load_result.moved_instructions == 1);
    EXPECT_TRUE(load_result.instructions[0].getOpcode() == "ld");
    EXPECT_TRUE(load_result.instructions[1].getOpcode() == "addi");
    EXPECT_TRUE(load_result.instructions[2].getOpcode() == "add");
    EXPECT_TRUE(load_result.instructions[0].getInstructionIndex() == 10);
    EXPECT_TRUE(load_result.instructions[1].getInstructionIndex() == 11);
    EXPECT_TRUE(load_result.instructions[2].getInstructionIndex() == 12);

    // General ALU RAW dependency: move an independent instruction into the gap.
    std::vector<ICUnit> alu_block = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "add", "x2", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x0")};
    auto alu_result = scheduleBasicBlock(alu_block, 3);
    EXPECT_TRUE(alu_result.moved_instructions == 1);
    EXPECT_TRUE(alu_result.instructions[1].getOpcode() == "addi");
    EXPECT_TRUE(alu_result.instructions[2].getOpcode() == "add");

    // The compatibility API is deliberately load-use-only; ordinary RAW
    // scheduling belongs to scheduleBasicBlock().
    const auto alu_load_use_result = scheduleLoadUseHazards(alu_block, 3);
    EXPECT_EQ(alu_load_use_result.moved_instructions, 0u);

    // WAR dependency: a candidate that overwrites the consumer's input is unsafe.
    std::vector<ICUnit> war_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "addi", "x1", "x6")};
    EXPECT_TRUE(scheduleBasicBlock(war_block, 3).moved_instructions == 0);

    // Candidate that consumes the producer's output is not an independent filler.
    std::vector<ICUnit> raw_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x1")};
    EXPECT_TRUE(scheduleBasicBlock(raw_block, 3).moved_instructions == 0);

    // Memory operations remain conservative barriers.
    std::vector<ICUnit> memory_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "lw", "x5", "x6")};
    EXPECT_TRUE(scheduleBasicBlock(memory_block, 3).moved_instructions == 0);

    // Window boundary: 2 disables scheduling; 3 permits one candidate.
    EXPECT_TRUE(scheduleBasicBlock(load_block, 2).moved_instructions == 0);
    EXPECT_TRUE(scheduleBasicBlock(load_block, 3).moved_instructions == 1);

    // A branch terminates its block, and its target begins a separate block.
    // Scheduling is allowed before the branch but never across it or the target.
    std::vector<ICUnit> program = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "add", "x2", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x0"),
        makeInstruction(3, "beq", "", "x2", "x0"),
        makeInstruction(4, "addi", "x6", "x0"),
        makeInstruction(5, "addi", "x7", "x0")};
    program[3].setLabel("target");
    auto program_result = scheduleProgramBlocks(program, {5}, 3);
    EXPECT_TRUE(program_result.moved_instructions == 1);
    EXPECT_TRUE(program_result.instructions[0].getOpcode() == "addi");
    EXPECT_TRUE(program_result.instructions[1].getOpcode() == "addi");
    EXPECT_TRUE(program_result.instructions[2].getOpcode() == "add");
    EXPECT_TRUE(program_result.instructions[3].getOpcode() == "beq");
    EXPECT_TRUE(program_result.instructions[4].getOpcode() == "addi");
    EXPECT_TRUE(program_result.instructions[5].getOpcode() == "addi");

    // Control-flow instruction remains in place; no candidate may cross it.
    std::vector<ICUnit> branch_barrier = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "beq", "", "x1", "x0"),
        makeInstruction(2, "addi", "x2", "x0"),
        makeInstruction(3, "addi", "x3", "x0")};
    branch_barrier[1].setLabel("target");
    auto barrier_result = scheduleProgramBlocks(branch_barrier, {2}, 4);
    EXPECT_TRUE(barrier_result.instructions[1].getOpcode() == "beq");

    // Default window setting remains configurable and input is not mutated.
    const std::size_t old_window = Kites::optimizer::g_scheduler_window_size;
    Kites::optimizer::g_scheduler_window_size = 3;
    EXPECT_TRUE(scheduleLoadUseHazards(load_block).moved_instructions == 1);
    Kites::optimizer::g_scheduler_window_size = old_window;
    EXPECT_TRUE(load_block[1].getOpcode() == "add");
    EXPECT_TRUE(load_block[2].getOpcode() == "addi");

}
