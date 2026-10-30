
#include "optimizer/instruction_scheduler.h"

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

TEST(InstructionSchedulerTest, SchedulesDependenciesAndPreservesInstructions)
{
    std::vector<ICUnit> alu_block = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "add", "x2", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x0")};

    const auto result = scheduleBasicBlock(alu_block);

    ASSERT_EQ(result.instructions.size(), alu_block.size());
    EXPECT_EQ(result.instructions[0].getOpcode(), "addi");
    EXPECT_EQ(result.instructions[1].getOpcode(), "addi");
    EXPECT_EQ(result.instructions[2].getOpcode(), "add");
    EXPECT_EQ(result.moved_instructions, 1u);

    // The input must remain unchanged.
    EXPECT_EQ(alu_block[1].getOpcode(), "add");
    EXPECT_EQ(alu_block[2].getOpcode(), "addi");
}

TEST(InstructionSchedulerTest, PreservesLoadUseDependency)
{
    std::vector<ICUnit> block = {
        makeInstruction(10, "ld", "x1", "x2"),
        makeInstruction(11, "add", "x3", "x1", "x4"),
        makeInstruction(12, "addi", "x5", "x6")};

    const auto result = scheduleBasicBlock(block);

    ASSERT_EQ(result.instructions.size(), 3u);
    EXPECT_EQ(result.instructions[0].getOpcode(), "ld");
    EXPECT_EQ(result.instructions[1].getOpcode(), "addi");
    EXPECT_EQ(result.instructions[2].getOpcode(), "add");
}

TEST(InstructionSchedulerTest, PreservesRegisterDependencies)
{
    // WAR: the candidate overwrites a register consumed by the dependent add.
    std::vector<ICUnit> war_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "addi", "x1", "x6")};

    EXPECT_EQ(scheduleBasicBlock(war_block).moved_instructions, 0u);

    // RAW: the candidate itself consumes the load's result.
    std::vector<ICUnit> raw_block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x1")};

    EXPECT_EQ(scheduleBasicBlock(raw_block).moved_instructions, 0u);
}

TEST(InstructionSchedulerTest, KeepsMemoryOperationsConservative)
{
    std::vector<ICUnit> block = {
        makeInstruction(0, "ld", "x1", "x2"),
        makeInstruction(1, "add", "x3", "x1", "x4"),
        makeInstruction(2, "lw", "x5", "x6")};

    EXPECT_EQ(scheduleBasicBlock(block).moved_instructions, 0u);
}

TEST(InstructionSchedulerTest, CompatibilityWrapperUsesBasicBlockScheduler)
{
    std::vector<ICUnit> block = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "add", "x2", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x0")};

    const auto basic_result = scheduleBasicBlock(block);
    const auto compatibility_result = scheduleLoadUseHazards(block);

    ASSERT_EQ(compatibility_result.instructions.size(), basic_result.instructions.size());
    EXPECT_EQ(compatibility_result.moved_instructions, basic_result.moved_instructions);

    for (std::size_t i = 0; i < basic_result.instructions.size(); ++i)
        EXPECT_EQ(compatibility_result.instructions[i].getOpcode(),
                  basic_result.instructions[i].getOpcode());
}

TEST(InstructionSchedulerTest, RespectsControlFlowBoundaries)
{
    std::vector<ICUnit> program = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "add", "x2", "x1", "x4"),
        makeInstruction(2, "addi", "x5", "x0"),
        makeInstruction(3, "beq", "", "x2", "x0"),
        makeInstruction(4, "addi", "x6", "x0"),
        makeInstruction(5, "addi", "x7", "x0")};

    program[3].setLabel("target");

    const auto result = scheduleProgramBlocks(program, {5});

    ASSERT_EQ(result.instructions.size(), program.size());
    EXPECT_EQ(result.instructions[0].getOpcode(), "addi");
    EXPECT_EQ(result.instructions[1].getOpcode(), "addi");
    EXPECT_EQ(result.instructions[2].getOpcode(), "add");
    EXPECT_EQ(result.instructions[3].getOpcode(), "beq");
}

TEST(InstructionSchedulerTest, KeepsBranchInPlace)
{
    std::vector<ICUnit> program = {
        makeInstruction(0, "addi", "x1", "x0"),
        makeInstruction(1, "beq", "", "x1", "x0"),
        makeInstruction(2, "addi", "x2", "x0"),
        makeInstruction(3, "addi", "x3", "x0")};

    program[1].setLabel("target");

    const auto result = scheduleProgramBlocks(program, {2});

    ASSERT_EQ(result.instructions.size(), program.size());
    EXPECT_EQ(result.instructions[1].getOpcode(), "beq");
}
