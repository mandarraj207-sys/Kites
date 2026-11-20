#include "optimizer/jalr_handler.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Kites::optimizer {
namespace {
std::optional<unsigned> regIndex(const std::string& name) {
    if (name == "zero") return 0;
    if (name == "ra") return 1;
    if (name.size() < 2 || name[0] != 'x') return std::nullopt;
    try {
        std::size_t used = 0;
        const auto value = std::stoul(name.substr(1), &used);
        if (used != name.size() - 1 || value > 31) return std::nullopt;
        return static_cast<unsigned>(value);
    } catch (...) { return std::nullopt; }
}

std::optional<std::int64_t> parseImm(const ICUnit& ins) {
    try {
        const std::string text = ins.getImm();
        std::size_t used = 0;
        const auto value = std::stoll(text, &used, 0);
        if (used != text.size()) return std::nullopt;
        return value;
    } catch (...) { return std::nullopt; }
}

bool isStandardReturn(const ICUnit& ins) {
    const auto rd = ins.getRd();
    const auto rs1 = ins.getRs1();
    return ins.getOpcode() == "jalr" &&
           (rd == "x0" || rd == "zero") &&
           (rs1 == "x1" || rs1 == "ra") && ins.getImm() == "0";
}
} // namespace

JalrAnalysis analyzeJalrTargets(const std::vector<ICUnit>& instructions) {
    JalrAnalysis result;
    result.protectedEntry.assign(instructions.size(), false);
    std::vector<std::optional<std::int64_t>> known(32);
    known[0] = 0;

    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const ICUnit& ins = instructions[i];
        const std::string op = ins.getOpcode();

        // Stage 1: protect the continuation written to ra by a conventional call.
        if (op == "jal" && (ins.getRd() == "x1" || ins.getRd() == "ra") &&
            i + 1 < instructions.size())
            result.protectedEntry[i + 1] = true;

        if (op == "jalr") {
            if (isStandardReturn(ins)) {
                ++result.standardReturns;
                continue;
            }

            // Stage 2: resolve rs1 + immediate when rs1 has a known value.
            const auto rs1 = regIndex(ins.getRs1());
            const auto imm = parseImm(ins);
            if (rs1 && imm && known[*rs1]) {
                const std::int64_t target =
                    (known[*rs1].value() + *imm) & ~std::int64_t{1};
                if (target >= 0 && target % 4 == 0) {
                    const auto targetIndex = static_cast<std::size_t>(target / 4);
                    if (targetIndex < instructions.size())
                        result.protectedEntry[targetIndex] = true;
                    ++result.resolvedIndirectJumps;
                    continue;
                }
            }

            // Stage 3: unknown target could be any instruction: preserve order globally.
            ++result.unresolvedIndirectJumps;
            result.disableScheduling = true;
            continue;
        }

        const auto rd = regIndex(ins.getRd());
        if (!rd || *rd == 0) continue;
        known[*rd].reset();

        if (op == "lui") {
            if (const auto imm = parseImm(ins))
                known[*rd] = static_cast<std::int64_t>(static_cast<std::uint32_t>(*imm) << 12);
        } else if (op == "auipc") {
            if (const auto imm = parseImm(ins))
                known[*rd] = static_cast<std::int64_t>(i * 4) + (*imm << 12);
        } else if (op == "addi") {
            const auto rs = regIndex(ins.getRs1());
            const auto imm = parseImm(ins);
            if (rs && imm && known[*rs]) known[*rd] = *known[*rs] + *imm;
        } else if (op == "add" || op == "sub") {
            const auto rs1 = regIndex(ins.getRs1());
            const auto rs2 = regIndex(ins.getRs2());
            if (rs1 && rs2 && known[*rs1] && known[*rs2])
                known[*rd] = op == "add" ? *known[*rs1] + *known[*rs2]
                                           : *known[*rs1] - *known[*rs2];
        }
    }
    return result;
}
} // namespace Kites::optimizer
