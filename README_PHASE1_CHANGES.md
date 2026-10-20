# KITES Phase 1 — Contribution and Change Log

> **Purpose:** This document distinguishes our Phase 1 extension work from the original KITES codebase. Update the status labels below as each change is applied and verified.
>
> **Important:** KITES is an existing project. Do not imply that the entire repository was written by us. Attribute only the extensions and modifications listed here, and follow the course/institute rules for acknowledging the upstream project.

## 1. Project goal

Phase 1 adds a hazard-aware instruction-reordering workflow to KITES. The scheduler analyzes a sequence of assembled instructions, looks for legal opportunities to move independent instructions around dependencies, and presents an optimized sequence for comparison with the original.

The primary validated target is **H_NF (hazard detection, no forwarding)**, where data dependencies can introduce multiple stall cycles. **H_F (forwarding enabled)** is exposed as an experimental benchmark configuration, but it must not be described as fully validated until its scheduling model and results have been tested.

Phase 1 focuses on:
- instruction dependency analysis and legal reordering;
- preserving program behavior;
- comparing original and optimized execution;
- measuring cycles, stalls, retired instructions and CPI;
- making benchmark results understandable in the UI.

Out of scope for Phase 1: implementing a new processor, changing the underlying hazard policy just to improve benchmark numbers, cache modelling, and custom-instruction/fusion execution.

## 2. Changes made in our extension

### A. Instruction scheduler

**Main files**
- `src/optimizer/instruction_scheduler.h`
- `src/optimizer/instruction_scheduler.cpp`
- `src/optimizer/scheduler_config.h` (if present in the current checkout)

**Contribution:** Added the optimizer/scheduler module for analyzing instruction dependencies and proposing legal reorderings. Keep the scheduler as a separate module rather than mixing its logic into the processor implementation.

**Status:** Confirm the files in the current working tree and tests before claiming every behavior is complete.

### B. Optimizer UI

**Main files**
- `src/ui/optimizer_tab/optimizertab.h`
- `src/ui/optimizer_tab/optimizertab.cpp`

**Contribution:** Added the Optimizer UI workflow for viewing the instruction sequence, requesting an optimized sequence, and connecting the optimizer to the rest of the application.

### C. Main-window integration and benchmark workflow

**Main files**
- `src/ui/mainwindow/mainwindow.cpp`
- `src/ui/mainwindow/mainwindow.h`

**Contribution:** Integrated the Optimizer UI and connected the original/optimized benchmark workflow to the processor manager. The benchmark should execute both versions under the same processor configuration so the comparison is meaningful.

The benchmark metrics include:
- cycles;
- stall cycles;
- instructions retired;
- CPI (cycles per instruction).

### D. Stall-cycle accounting

**Files to check**
- `src/processor/processor_base.h`
- `src/processor/rv5s/rv5s_processor_h_nf.cpp`
- `src/processor/rv5s/rv5s_processor_h_f.cpp`
- `src/processor/processor_manager.cpp` (or the current manager implementation)

**Contribution:** Added a cumulative stall-cycle counter and exposed it to benchmark metric collection.

**Caution:** This is an instrumentation/measurement change. The existing hazard detector and processor semantics should remain unchanged unless a separately documented bug fix is justified and tested.

### E. End-of-program / breakpoint fix

**File**
- `src/processor/rv5s/rv5s_processor_base.cpp`

**Contribution:** A patch was applied to prevent an end-of-program breakpoint from interfering with pipeline draining. The intended behavior is to let already-fetched instructions finish through writeback before the run terminates.

**Verification status:** The user reported that the benchmark completed after the fix. Preserve the pipeline-drain condition; do not replace it with a forced early exit. Add a regression test if feasible.

### F. Benchmark configuration popup

**File**
- `src/ui/mainwindow/mainwindow.cpp`

**Contribution:** Added a prompt when starting a benchmark to choose between:
- **H_NF — No Forwarding (Phase 1)**;
- **H_F — Forwarding (Experimental)**.

The selected configuration should be applied consistently to both original and optimized runs. H_F being selectable does **not** mean its scheduler cost model or results have been validated.

**Build note:** The popup required `#include <QPushButton>` in `mainwindow.cpp` because `QMessageBox::addButton()` returns a `QPushButton*`. This include belongs in the `.cpp` file for the current implementation.

### G. Graphical benchmark results UI

**Intended file**
- `src/ui/optimizer_tab/optimizertab.cpp`

**Planned presentation**
- original vs optimized metrics;
- cycles and stalls;
- CPI;
- percentage changes;
- a clear processor-configuration label;
- a cautious correctness indicator based on the checks actually performed.

**Status:** Mark this section as **implemented** only after the graphical UI patch has been applied to the working repository, compiled successfully, and tested. If it has not been applied yet, leave it as **planned/pending**. A patch generated or downloaded elsewhere is not part of the repository until it is applied and committed.

## 3. Observed benchmark result

One test run reported:

| Metric | Original | Optimized |
|---|---:|---:|
| Cycles | 19 | 16 |
| Stall cycles | 6 | 3 |
| Instructions retired | 9 | 9 |
| CPI | 2.111 | 1.778 |

For that run:
- cycle reduction = `(19 - 16) / 19 × 100 ≈ 15.79%`;
- stall reduction = `(6 - 3) / 6 × 100 = 50%`;
- CPI values are consistent with cycles divided by nine retired instructions.

These are **observed results for one test**, not a guarantee that every program improves. The displayed final `x16` value was consistent with `x16 = x14 + x17` for the operands shown. This alone is not a full equivalence proof. Validate final architectural register/memory state and use more than one test before making broad correctness claims.

## 4. How to mark our code in the source

Do not add comments to every line. Instead, clearly mark the start and end of each substantial contribution, and explain the intent. Use consistent comments such as:

```cpp
// ===== KITES Phase 1 contribution: benchmark stall-cycle accounting =====
// Purpose: expose the number of stall cycles consumed by this processor.
// The surrounding pipeline/hazard behavior is inherited from the base project.
```

At the end of a substantial block, use:

```cpp
// ===== End KITES Phase 1 contribution =====
```

For the benchmark configuration dialog:

```cpp
// ===== KITES Phase 1 contribution: benchmark processor selection =====
// Both original and optimized runs must use the selected configuration.
// H_F is available experimentally; its optimizer behavior is not yet validated.
```

For the scheduler module, a file-level comment is suitable:

```cpp
/*
 * KITES Phase 1 extension: hazard-aware instruction scheduling.
 * This module is an addition to the existing KITES codebase.
 * It analyzes instruction dependencies and proposes legal reorderings.
 */
```

For the graphical results UI, document only the UI code we actually added. Avoid labeling an entire existing function or file as our work if only a small block changed.

**Do not claim authorship of inherited code.** When modifying an existing function, mark the specific inserted block and explain why it was added. If the instructor wants line-by-line provenance, use Git diffs/commits as the authoritative record.

## 5. Suggested commit organization

If the changes are currently mixed together, a single honest commit is acceptable. If they are easy to separate, use commits such as:

1. `feat(optimizer): add hazard-aware instruction scheduler`
2. `feat(ui): integrate optimizer workflow`
3. `feat(benchmark): collect cycle stall and CPI metrics`
4. `fix(benchmark): select processor configuration per run`
5. `feat(ui): present benchmark results graphically`
6. `docs: document Phase 1 contributions`

Do not create commits that claim changes not present in the working tree. The graphical UI commit should be made only after that patch has actually been applied and tested.

## 6. Build and test checklist

Before presenting Phase 1:
- [ ] `cmake --build build -j"$(nproc)"` succeeds.
- [ ] H_NF benchmark completes for original and optimized programs.
- [ ] Original and optimized runs use the same selected processor configuration.
- [ ] Both runs retire the expected number of instructions.
- [ ] Final register and memory state is compared, not just the instruction count.
- [ ] Stall counts are checked against cycle-by-cycle behavior or a known reference.
- [ ] Several programs are tested, including a dependency-heavy program and a program with few/no reorder opportunities.
- [ ] H_F is labelled experimental until separately validated.
- [ ] Documentation and screenshots reflect the version actually committed.

## 7. GitHub upload workflow

Run these commands from the project root:

```bash
cd "$HOME/kites source code/Kites-main"
git status --short
git remote -v
```

Check the output before proceeding:
- `git status --short` lists modified/untracked files.
- `git remote -v` shows the configured remote repository, if one exists.

Add the documentation and the source changes you intend to publish. Prefer explicitly naming files at first:

```bash
git add README_PHASE1_CHANGES.md
git add src/optimizer/
git add src/ui/optimizer_tab/
git add src/ui/mainwindow/mainwindow.cpp src/ui/mainwindow/mainwindow.h
git add src/processor/processor_base.h
git add src/processor/rv5s/rv5s_processor_h_nf.cpp
git add src/processor/rv5s/rv5s_processor_h_f.cpp
git add src/processor/processor_manager.cpp
git diff --cached --stat
git diff --cached
```

Some paths may differ in the current checkout. Only stage paths that exist and that contain your intended contribution. Review `git diff --cached` and make sure you are not accidentally uploading build outputs, local runtime state, editor backups, secrets, or unrelated changes.

Then commit:

```bash
git commit -m "feat: add Phase 1 hazard-aware instruction optimizer"
```

Push the current branch:

```bash
git branch --show-current
git push -u origin HEAD
```

If `git remote -v` shows no remote, do not guess a repository URL. Create/select the intended GitHub repository, copy its URL, and configure it:

```bash
git remote add origin <YOUR_GITHUB_REPOSITORY_URL>
git push -u origin HEAD
```

Replace the placeholder with the URL copied from your repository page. If the remote already exists, do not add it again.

### Important Git safety notes
- If this is a clone of someone else's upstream repository, check permission before pushing to its `origin`. You may need to push to your own fork instead.
- Do not force-push.
- Do not upload the `build/` directory or generated runtime files unless the project's contribution instructions explicitly require them.
- Keep upstream attribution and license notices intact.
- A Markdown change log is documentation, not a substitute for source comments, tests, or a clear Git diff.
