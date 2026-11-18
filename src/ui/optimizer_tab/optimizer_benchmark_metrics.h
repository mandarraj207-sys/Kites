#pragma once

namespace Kites {

struct OptimizerBenchmarkMetrics
{
    unsigned int cycles = 0;
    unsigned int stalls = 0;
    unsigned int instructionsRetired = 0;
    float cpi = 0.0f;
};

} // namespace Kites
