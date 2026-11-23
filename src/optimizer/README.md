# KITES optimizer — commented source edition

These are the 14 optimizer source/header files supplied in the conversation, with explanatory comments added.

## File responsibilities
- `dependency_graph.*`: builds and filters register RAW-dependency records.
- `forwarding_instruction_scheduler.*`: H_F forwarding-aware scoring policy and block-scheduling entry point.
- `instruction_scheduler.*`: H_NF no-forwarding scoring policy, optional trace helpers, and block/program scheduling entry points.
- `jalr_handler.*`: analyzes calls/returns and indirect-jump targets; protects sensitive entries or disables scheduling if a target cannot be resolved safely.
- `program_optimizer.*`: expands and assembles editor source, partitions safe regions, invokes H_F/H_NF, and rebuilds optimized program metadata and machine code.
- `scheduler_core.*`: shared instruction decoding, dependency graph, critical-path calculation, candidate selection, and region scheduling.
- `stall_model.*`: static linear pipeline-stall accounting for H_NF/H_F.

## Commenting policy
Comments describe module responsibilities, helper roles, public APIs, and debug-only code. Executable statements and scheduling policy are unchanged.
