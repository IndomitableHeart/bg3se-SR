# AGENTS.md

This file is for Codex and other coding agents working on BG3Access. It does not replace `CLAUDE.md`, and it should not change Claude's behavior unless the user explicitly chooses to point Claude here.

Shared project invariants that both Claude and Codex may use live in `AI_SHARED_RULES.md`.

## Codex Default Workflow

For BG3Access work, Codex should start with a read-only orientation pass unless the user explicitly asks for a small mechanical edit and the relevant context is already known.

Do not edit code during the orientation pass. First read the architecture guidance, task-specific code, and relevant memory notes, then summarize the current behavior and proposed plan.

## Required Orientation Sources

For broad BG3Access work, read:

- `CLAUDE.md`
- `AI_SHARED_RULES.md`
- `C:\Users\jlove\.claude\projects\D--Repositories-bg3se-SR\memory\MEMORY.md`
- task-relevant Claude memory files, especially `feedback_*.md`, `project_*.md`, and `reference_*.md`
- relevant sections of `BG3Extender\Lua\Libs\ClientUI\Module.inl`
- Lua entrypoints: `Client\EventRouter.lua`, `Client\Dispatcher.lua`, and `Client\SpeechData.lua`
- the task-specific Lua handler, such as `WorldUI.lua`, `WorldNav.lua`, `CharCreation.lua`, `Menus.lua`, `Combat.lua`, `DiceRolls.lua`, or related modules

This does not mean reading every file in the repository every time. It means reading every file that defines the architecture, constraints, contracts, and task-specific behavior before making changes.

## Before Editing

Before implementation, Codex should report:

- files read
- current behavior summary
- proposed files to edit
- invariants that must not be broken
- a test plan the user can run in BG3
- uncertainty or missing context

Wait for user approval before editing unless the user has already explicitly authorized implementation.

## Risky Changes

For risky or behavior-sensitive changes, use phases:

1. Phase 1: smallest mechanical change
2. pause for the user's in-game test
3. Phase 2: behavior refinement
4. pause again
5. Phase 3: cleanup and docs

## Agent Coexistence

- Do not modify `CLAUDE.md`, Claude memory files, or other agent-specific instructions unless the user explicitly asks.
- Do not rewrite another agent's work just to match Codex style.
- If another agent has made changes, read them carefully and work with them.
- Put shared architecture rules in `AI_SHARED_RULES.md` only when the user wants them to be shared.
- Put Codex-specific workflow rules in this file.

## Build Policy

Do not run command-line C++ builds for the Script Extender unless the user explicitly asks. The user builds the extender in Visual Studio 2022.

Lua-only changes may have in-game test plans instead of command-line build steps.