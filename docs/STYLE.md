# Writing style for the docs

Rules for every page, whether a person or an AI wrote the first draft. The goal: a beginner can follow every page without asking for help.

## Voice

- Write in **English**, to "you" (second person).
- Use short sentences. One idea each.
- Use plain words. Say "the screen" before "the display plane".
- Say what the reader should do first, why second.

## Structure

- One recommended path per page. Put alternatives in a `<details>` block or a short list below it.
- Tutorials (`getting-started.md`) end every step with **You should now see...** and a time estimate.
- How-to pages (`cameras.md`) start from a goal and end when it is done.
- Reference pages (`configuration.md`) are tables. No story.
- Explanations (`how-it-works.md`) use pictures and plain words. The details live in `architecture.md`, which is for contributors.
- State limits up front. Never bury them.

## Jargon

- Explain a technical word the first time you use it on a page, or link it to [glossary.md](glossary.md).
- Add new terms to the glossary in one or two sentences.

## Commands

- Every command block must be safe to paste. Check it.
- Use `sudo` where it is needed.
- **Never chain a reboot** after other commands. Put `sudo reboot` in its own block.
- Never put a password or URL with credentials on a command line. Use the hidden prompt of `probe` and `add`.
- Do not hardcode a version number. Use `releases/latest` links.

## Claims

- Do not write a claim you cannot support. If it is not tested, say "not tested".
- A performance number always states the board, the streams (size and frame rate), the display mode, and the CPU scale. Link to [benchmarks.md](benchmarks.md).
- Use the exact text the program prints when you quote a message. `docs/troubleshooting.md` must match the code.
- Competitor facts need a source and a date. Say where they win.
- Every command, flag and setting you mention must exist in the code. Check with `rtspwall --help` and `rtspwall COMMAND --help`.
- Never say "tested for N hours" unless a run of N hours is documented.

## Links and files

- Use relative links between pages. Check them before you commit.
- Do not add images that do not exist yet. Use an HTML comment as a placeholder.
- Do not include secrets, real camera addresses or real tokens in examples. Use `PASSWORD` and addresses like `192.168.1.10`.
