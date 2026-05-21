# CLAUDE.md - Coding Profile
# Best for: dev projects, code review, debugging, refactoring
# Extends: Universal CLAUDE.md rules

---

## Output
- Return code first. Explanation after, only if non-obvious.
- No inline prose. Use comments sparingly - only where logic is unclear.
- No boilerplate unless explicitly requested.

## Code Rules
- Simplest working solution. No over-engineering.
- No abstractions for single-use operations.
- No speculative features or "you might also want..."
- Read the file before modifying it. Never edit blind.
- No docstrings or type annotations on code not being changed.
- No error handling for scenarios that cannot happen.
- Three similar lines is better than a premature abstraction.

## Review Rules
- State the bug. Show the fix. Stop.
- No suggestions beyond the scope of the review.
- No compliments on the code before or after the review.

## Debugging Rules
- Never speculate about a bug without reading the relevant code first.
- State what you found, where, and the fix. One pass.
- If cause is unclear: say so. Do not guess.

## Git Discipline
- Every code change must be committed immediately after editing.
- If the change is a major algorithm swap or large-scale rewrite, create a new branch first, then commit.
- For algorithm changes, write the full method description to UPDATES.md (in Chinese) in the project root before committing.
- CLAUDE.md changes must be synced to all branches (commit on current branch, then cherry-pick to others).

## Build Environment
- ESP-IDF v5.5.4
- Activate with: `. ~/.espressif/tools/activate_idf_v5.5.4.sh`
- Build with: `idf.py build`
- Flash with: `idf.py -b 115200 flash`

## Simple Formatting
- No em dashes, smart quotes, or decorative Unicode symbols.
- Plain hyphens and straight quotes only.
- Natural language characters (accented letters, CJK, etc.) are fine when the content requires them.
- Code output must be copy-paste safe.
