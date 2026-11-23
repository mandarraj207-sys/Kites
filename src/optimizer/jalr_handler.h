#ifndef KITES_JALR_HANDLER_H
#define KITES_JALR_HANDLER_H

#include <cstddef>
#include <vector>
#include "assembler/code_generator.h"

namespace Kites::optimizer {
// Result model: protected entry points, resolution counts, and scheduling-safety status.
struct JalrAnalysis {
    bool disableScheduling = false;
    std::vector<bool> protectedEntry;
    std::size_t standardReturns = 0;
    std::size_t resolvedIndirectJumps = 0;
    std::size_t unresolvedIndirectJumps = 0;
};

// API: analyze indirect jumps and identify instruction indices that must remain fixed.
JalrAnalysis analyzeJalrTargets(const std::vector<ICUnit>& instructions);
}
#endif
