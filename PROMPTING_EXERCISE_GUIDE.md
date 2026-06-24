# Porting MS-DOS 1.25 with Claude

A hands-on course in **using Claude to work with an unfamiliar codebase** —
specifically, porting MS-DOS 1.25 from 8086 assembly to C.

The assembly in `v1.25/source/` is roughly 12 000 lines of 1982-era code in a
language most developers have never written.  That is the point.  You are not
expected to learn 8086 assembly.  The exercises here are about developing the 
habits that let you use Claude productively on *any* codebase you do
not fully understand.

Each exercise has:
- a **prompting objective** — the communication skill being practised
- a **task** — what you are trying to accomplish
- a **starting prompt** — a short, natural prompt you could type right now
- **hints** — small additions that steer toward a more reliable result
- a **quality bar** — what a useful response looks like
- **iteration cues** — signals that you need a follow-up prompt

Work through these in order.  Each exercise's output becomes input to the next.

---

## Attribution

The assembly source used in these exercises is the original **MS-DOS 1.25**,
written by **Tim Paterson** at Seattle Computer Products in 1982.  Microsoft has
published it at:

> **[https://github.com/microsoft/ms-dos](https://github.com/microsoft/ms-dos)**

It is made available under the **MIT License**.  The C port you produce in these
exercises is an independent reimplementation written from scratch using the
assembly as a specification, and may also be released under the MIT License.

---

## What You Are Building

The original MS-DOS 1.25 is made up of three distinct parts.  The port follows
the same structure:

**The kernel** (`MSDOS.ASM`) is the core of the operating system.  It manages
the FAT12 filesystem — reading and writing clusters on disk, tracking which
clusters belong to which file, and finding free space.  It also implements the
system call interface that programs use to open files, read and write data, and
query the disk.  The kernel knows nothing about the terminal or the host machine;
all hardware access goes through an abstraction layer.

**The command processor** (`COMMAND.ASM`) is the interactive shell — the program
that prints a prompt, reads commands, and runs them.  It sits on top of the
kernel and calls the kernel's system calls to do its file operations.  It is also
where new shell commands are added.

**The host wrapper** (replaces `IO.ASM`) is the layer that connects the kernel
to the real hardware of the machine you are running on.  In the original it spoke
directly to PC hardware; in the C port it speaks to the terminal using POSIX APIs
and stores the disk as a file on the host filesystem.

**The disk image** is a single binary file that contains a complete FAT12
filesystem — the same format that a physical floppy disk would have.  The wrapper
opens it on start and flushes it on exit.  Everything the shell does to "disk" is
actually a read or write inside this file.  Creating a blank image and the supported sizes are things you can ask for when
building the host wrapper in Exercise 4.

---

## Before You Start

### Getting the exercise workspace

The starting point for all exercises is packaged as **`exercise-workspace.zip`**,
located in the root of this repository.

To begin, decompress it into a fresh working directory:

```sh
unzip exercise-workspace.zip -d my-msdos-port
cd my-msdos-port
```

This gives you a `v1.25/` folder containing the original MS-DOS 1.25 assembly
source and companion files — everything you need to work through the exercises
from scratch.  Do not copy anything from `src/` or `tests/`; those are the
finished reference implementation and should only be consulted if you get
completely stuck.

Once extracted, follow the **Start with a git repository** step below before
writing any code.

### What is in the repository

```
v1.25/source/      8086 assembly — the original MS-DOS 1.25
src/               C port — the finished result you are working toward
tests/             Test suite for the port
claude-prompts.md  The one-line prompts used to build this project
```

Keep `src/` as a fallback if you get completely stuck, not as a starting point.

Read `claude-prompts.md` once.  Those prompts are intentionally terse — they
worked because of accumulated context built up across a long session.  This
guide teaches you to get reliable results from a standing start.

### Start with a git repository

Before writing any code, run `git init` in your project folder and make an
initial commit.  This gives you a safety net for every exercise: if a prompt
produces a result that breaks something that was working, you can see exactly
what changed (in VS Code's Source Control panel or with `git diff`) and revert
to the last good state with a single command.

Get into the habit of committing after each exercise completes and its tests
pass.  A commit history also gives Claude useful context — you can ask "what
changed since the last commit?" to orient a new session quickly.

### Ground rules

1. **You direct; Claude executes.** You do not need to read or understand the
   source code — Claude will.  Your job is to give it the right goal, the right
   constraints, and to test and observe the result.

2. **Test after every prompt.** Build and run the output.  What you observe is
   your main source of follow-up questions — not the code itself.

3. **One concern per prompt.** Mixing two tasks in one message produces output
   that is hard to test and harder to fix.

4. **Use a review prompt when output is large.** Before accepting a substantial
   result, ask Claude to review it.  See *The Review Prompt* section below.

5. **Keep a living README and REFERENCE.md.** After each exercise, ask Claude to
   update both files to reflect what has been built.  See *Living Documentation*
   below.

6. **Commit when tests pass.** A passing commit is a safe rollback point.  If
   the next prompt breaks something, `git checkout .` gets you back instantly.

7. **Use plan mode for complex exercises.** Before asking Claude to write a
   large or high-stakes piece of code, switch into plan mode (Shift+Tab in
   Claude Code, or ask "think through the approach before writing any code").
   Claude will reason through the design, surface trade-offs, and wait for your
   approval before producing anything.  This is especially useful for the kernel
   port and the BASIC interpreter — two exercises where a structural mistake is
   expensive to undo.  Once you are happy with the plan, confirm it and Claude
   will implement it in one coherent pass.

8. **Match the model to the task.** Not every prompt needs the most powerful
   model.  A rough guide:
   - **Opus / high effort** — analysis, documentation, architecture decisions,
     the translation reference, and any prompt where the answer will be reused
     many times.  Worth the extra cost when the output shapes everything that
     follows.
   - **Sonnet / medium effort** — implementation, adding commands, writing tests,
     fixing bugs.  Fast enough to iterate quickly and capable enough for the
     work.  Use this for the majority of prompts.

---

## The Review Prompt

When Claude produces a large result — a new module, a complex command, a test
suite — do not just compile it and hope for the best.  Ask Claude to review its
own output before you run it:

> "Review what you just wrote.  What are the most likely problems or edge cases
> you may have missed?"

This works because Claude can reason about its output more critically when asked
directly.  A good review response will surface at least one real issue — handle
it before moving on.

You can also use a targeted review when something feels off after testing:

> "I ran it and observed [X].  Review the relevant part of the code and explain
> why it might behave that way."

The review prompt is not a substitute for running the code — it is a complement.
Use it after every exercise that produces non-trivial output.

---

## Living Documentation

As the project grows across multiple sessions, Claude loses track of what has
already been built.  A `README.md` and a `REFERENCE.md` that stay up to date solve
this: at the start of any new session you paste them in as context — or simply
ask Claude to read them: "Read REFERENCE.md and README.md before we continue." —
and Claude immediately knows the current state of the project.

After each exercise, ask:

> "Update README.md and REFERENCE.md to reflect what was just added."

Then run `/compact` before starting the next exercise.  Each exercise generates
a lot of back-and-forth — code, test output, fixes, reviews — and the context
window fills up quickly.  `/compact` summarises the conversation so far and
discards the detail, keeping only what matters for the next step.  With
up-to-date `REFERENCE.md` and `README.md` files already committed, almost nothing
important is lost: Claude can read those files to reconstruct the project state
at any point.

`README.md` should describe the project for a user: what it does, how to build
it, and which commands are available.  `REFERENCE.md` should describe the project
for Claude: the architecture, the key design decisions, what files exist and what
they do, and any rules that must be kept (such as the no-direct-I/O rule in the
kernel).

A good `REFERENCE.md` entry after Exercise 3 might read: *"The kernel in `src/kernel/`
must never call printf, fread, or any POSIX function directly.  All I/O goes
through the `bios_t` vtable defined in `bios.h`."*  That rule will then be
applied consistently in every future session without you having to repeat it.

---

## Exercise 1 — Get a Map of the Codebase

### Prompting objective
Ask Claude to turn an unfamiliar source tree into something you can navigate.

### Task
Understand what the source files in `v1.25/source/` do and how they relate.

### Starting prompt

> "Analyse and describe the contents of the `v1.25/source/` folder in a document called REFERENCE.md placed in the root of the workspace."

### Hints
That prompt works but tends to produce a long prose description.  Two small
additions make the output more useful:

- Ask for a **specific format**: "describe it as a table — one row per file,
  showing its responsibility, what it provides, and what it depends on."
- Ask for a **dependency diagram**: "add a short diagram showing which files
  call into which."

Ask Claude to save the result as `REFERENCE.md` in the repository so it can be
used as context in future sessions.

### Quality bar
The output should name specific roles ("FAT12 cluster allocation", "keyboard and
screen I/O") not vague ones ("handles files").  If a description feels generic,
follow up: "What does this file do that no other file does?"

### Iteration cues
- If two files seem to overlap, ask: "What is the boundary between [A] and [B]?"
- Keep this table — you will refer back to it throughout the project.

---

## Exercise 2 — Build a Translation Reference

### Prompting objective
Generate a reusable document before a large task so Claude applies consistent
choices throughout, rather than reinventing them in each function.

### Task
Create a reference that maps 8086 assembly patterns to C equivalents.

### Starting prompt

> "Create a reference document of 8086 assembly that can later be used to port
> this project to C code. Place it in the root of the workspace."

### Hints
The starting prompt gets you a general reference.  Add one constraint to make it
useful specifically for this port:

- "The kernel must never call any C standard library I/O function directly.
  All console and disk access goes through a struct of function pointers."

This single rule prevents a class of design errors that are painful to fix later.
Ask Claude to include it explicitly in the document so you can read it into
future prompts as a reminder.

After getting the document, use the review prompt:

> "Review this reference document.  Is there anything important for porting an
> OS kernel to C that is missing?"

### Quality bar
The hardware abstraction section should define a C struct with function pointers,
not concrete function implementations.  If you see `printf` or `fread` anywhere
in a kernel function later, the abstraction rule was not applied.

### Iteration cues
- Save this document as a file in the repository.  At the start of each
  subsequent session, paste it in or ask Claude to read it: "Read the 8086-to-C
  reference document before we continue."
- If Claude makes inconsistent choices later, point back to the reference:
  "The reference says [X].  Revise to match it."

---

## Exercise 3 — Write the Host Wrapper

### Prompting objective
Describe a component by what it must do, not how to implement it.

### Background
The wrapper is the only part that talks to the real machine.  It implements the
vtable the kernel calls through — terminal input/output and disk sector reads and
writes — and contains `main()`, which opens the disk image, runs the shell, and
flushes everything on exit.  The disk image is a raw binary file in standard
FAT12 format: a boot sector with geometry info, two FAT copies, a root directory,
and data sectors, exactly as a real floppy disk would be laid out.

Building the wrapper first means the kernel can be ported and tested against a
real running system from the start, rather than in isolation.

### Task
Write the POSIX layer that makes the kernel run on a modern system — terminal
input/output and disk image files — plus the program entry point.

### Starting prompt

> "Write a wrapper CLI application in C called msdos. It should handle keyboard input and screen output, and support disk reads and writes to a disk image file that is loaded on start and saved on exit. The purpose of the wrapper is to run the ported msdos in this wrapper. The wrapper must behave as the host of the operating system being ported."

### Hints
One addition avoids a common pitfall with the editor you will add later:

- "Use raw terminal mode for keyboard input so the program receives every
  keypress directly, including control keys."

Without this, arrow keys and control characters will not work in the editor.

You will also need a disk image to test with.  Ask for it as part of this exercise:

- "Also add a `--format path --720` flag that creates a blank FAT12 disk image.
  Support `--180`, `--360`, and `--720` for different sizes."

The three sizes correspond to common floppy disk capacities; `--720` is the most
useful for testing as it gives the most space.

Ask Claude to also produce a `Makefile` in the **root of the repository** with
at minimum a `make` target that builds the binary and a `make test` target that
runs the test suite:

- "Add a Makefile in the root directory with targets to build the project and
  run the tests."

Having a Makefile from the start means every subsequent exercise can be verified
with the same two commands: `make` and `make test`.

Once the wrapper compiles and formats a disk, ask for basic tests to confirm the
wrapper itself is sound:

- "Write tests for the wrapper that verify: a freshly formatted disk image has
  the correct size, the boot sector contains a valid FAT12 signature, and opening
  a non-existent image file produces an error rather than a crash."

After completing this exercise, ask Claude to update `README.md` with build
instructions and the `--format` usage, and update `REFERENCE.md` with a description
of the wrapper's role and file location.

### Quality bar
Running `make` should produce a binary.  Running `./msdos disk.img` should reach
a prompt.  Running `make test` should run and pass the wrapper tests.

### Iteration cues
- If the program exits immediately, ask: "What should happen step by step between
  launch and the first prompt appearing?  Where might it be stopping early?"
- If the disk image is rejected on load, ask: "What does the program check in the
  disk image when it opens it?"
- If `make` fails, ask: "What does the Makefile need to find in order to build?
  Is the source file layout what it expects?"

---

## Exercise 4 — Port the Kernel

### Prompting objective
Scope a large porting task into layers you can test one at a time.

### Background
This is the exercise with the least back-and-forth, but the most important to get
right.  Bugs in the kernel — a cluster walk that skips an entry, a directory scan
that stops too early — will not surface immediately.  They will appear later as
mysterious failures in the editor, the BASIC interpreter, or the subdirectory
commands, and by then Claude has no clear trail leading back to the root cause.
Getting the kernel correct now prevents a class of debugging sessions that are
expensive and hard to reason about.

The kernel manages the FAT12 filesystem and exposes a system call interface to
the shell.  It knows nothing about the terminal or the host — all hardware access
goes through the vtable defined in Exercise 2 and implemented in Exercise 3.
Inside it is layered: FAT arithmetic at the bottom, disk buffering, file control,
and the system call dispatcher at the top.  Porting bottom-up means each layer is
tested before anything builds on it.

### Task
Port `MSDOS.ASM` to C.

### Starting prompt

> Port the operating system kernel in MSDOS.ASM to C. Use the 8086 assembly to c document as the translation guide. keep the kernal compact, functional and maintain the scope of the MSDOS.ASM. write test cases to verify the created kernel. integrate the kernel with the wrapper.

### Hints
This is a good exercise for plan mode.  Before writing any code, ask Claude to
think through the layering and file structure: "Think through how to structure
the kernel port before writing any code."  Review the plan, push back on anything
that feels off, then confirm — Claude will implement in one coherent pass rather
than accumulating half-decisions across many prompts.

"Port the kernel" is a large task.  Two additions keep it manageable:

- Ask Claude to **start with the lowest layer**: "begin with the FAT12 arithmetic
  functions — they have no dependencies and can be tested in isolation." Note that this takes considerably more time.
- Ask for **tests before moving up**: "write unit tests for each layer before
  starting the next one."

This creates a bottom-up rhythm: FAT arithmetic → disk buffering → file operations
→ the system call dispatcher.  Run the tests at each layer before moving on.

After each layer, use the review prompt before running it:

> "Review the layer you just wrote.  What are the most likely correctness
> problems?"

When the kernel is complete, ask Claude to update `REFERENCE.md` with the file
layout, the vtable rule, and a brief description of each kernel layer.

### Quality bar
Each layer's tests should pass before the next layer starts.  No kernel file
should call `printf`, `fread`, or any other I/O function from the C standard
library directly.

### Iteration cues
After each layer passes its tests, ask Claude to review it before moving on:

> "Review the code you just wrote for correctness and conciseness.  Are there any
> functions that could be simplified, and are there any edge cases that are not
> handled?"

A good review response will often surface at least one real issue — a missing
bounds check, a cluster walk that does not handle an empty file, or a function
that is longer than it needs to be.  Fix those before starting the next layer.

---

## Exercise 5 — Build the Command Processor

### Prompting objective
Describe behaviour precisely so Claude does not have to guess what the user sees.

### Background
The command processor is the interactive shell.  It calls into the kernel for all
file operations and has no direct access to the disk.  Commands are dispatched
through an internal table; anything not found falls back to a "bad command"
message (the original would attempt to run a `.COM` file, but this port has no
x86 emulator).

### Task
Port `COMMAND.ASM` to produce an interactive shell with file commands.

### Starting prompt

> "Port the command processor in `COMMAND.ASM` to C.  It should show a prompt,
> read a command, and dispatch to the right handler."

### Hints
Porting `COMMAND.ASM` will bring the original command set with it: `DIR`,
`TYPE`, `COPY`, `DEL`, `ERASE`, `REN`, `RENAME`, `DATE`, `TIME`, `PAUSE`, and
`REM`.  The follow-up work is verification, not addition.  Test each command and
describe what you observe if something is off:

- Try `DIR` on an empty disk and on a disk with files.
- Try `DEL *.*` and confirm it asks for confirmation before deleting everything.

If a command is missing or behaves unexpectedly, describe the gap: "DIR is
present but does not show free bytes at the end.  Can you add that?"  Ask for
one fix at a time rather than listing everything at once.

Commands that were *not* in the original — `ECHO`, `VER`, `CLS`, and `CHKDSK`
— will need to be asked for separately.  A single follow-up prompt is enough:
"Add ECHO, VER, CLS, and CHKDSK commands."  `HELP` is covered as its own
exercise next.

After the commands are working, ask Claude to update `README.md` with the command
list and update `REFERENCE.md` with the command table structure so future sessions
know how to add new commands.

### Quality bar
`DIR` on an empty disk should show zero files and a correct free-space number.
`COPY` followed by `DIR` should show the new file with the right size.

### Iteration cues
- If a command produces wrong output: "I ran [command] and got [output].  I
  expected [X].  What would cause that difference?"
- If a command crashes: "Running [command] with no argument causes a crash.
  What should it do instead?"

---

## Exercise 6 — Add a New Command: HELP

### Prompting objective
Write a minimal prompt for a well-scoped addition — just enough to be unambiguous.

### Task
Add a `HELP` command that lists all available commands with a short description
of each.

### Starting prompt

> "Add a HELP command with all commands supported in this DOS version."

### Hints
That is enough to get a working result.  One thing worth checking after: does
typing `HELP` alone list all commands, and does `HELP DIR` (or similar) show
detail for a single command?  If only one of those works, describe what you
observed and ask for the other form.

This exercise also introduces a useful pattern for any future renaming or
reorganisation: before making a change that touches a name in multiple places,
ask "list every place [name] appears in the codebase" first, then ask for the
change in a second prompt.

After adding HELP, ask Claude to update `README.md` to include the new command,
and remind it to keep `README.md` current after every addition from this point on.

### Quality bar
`HELP` should list every command the shell supports.  If a command you added
earlier is missing from the output, point it out: "HELP does not list [command].
Can you add it?"

### Iteration cues
- If the HELP output is very long and hard to read, ask: "Can the output be
  formatted into two columns?"
- If Claude changes more than expected, ask: "Was that change needed for HELP,
  or is it a separate concern?"

---

## Exercise 7 — Add a Complex Command: Full-Screen Editor

### Prompting objective
Ask about constraints before asking for code when a feature has non-obvious
failure modes.

### Task
Add an `EDIT` command — a full-screen text editor backed by the FAT12 disk.

### Starting prompt

> "Add an EDIT command that lets the user view, edit, and save a text file on
> disk."

### Hints
A terminal editor has failure modes that only appear at runtime.  Ask one
question before any code is written:

> "Before writing any code: what are the most common ways a terminal editor
> breaks on a POSIX system that I should design around from the start?"

Take the answer seriously.  Then when asking for the implementation, add:

- "Use key bindings that the terminal will not intercept."

This one phrase steers Claude toward safe choices and away from control characters
that terminals silently consume.

After receiving the implementation, use the review prompt:

> "Review the editor implementation.  What are the most likely problems with key
> handling or file save/load that I should test first?"

Then test in that order.  Once the editor is working, ask Claude to update
`REFERENCE.md` with how the editor is structured and note any key binding decisions
that were made — these are easy to forget and painful to rediscover.

### Quality bar
Test by opening a file, making a change, saving, and reopening — the change
should be there.  Arrow keys, Home, End, and Escape should all behave correctly.

### Iteration cues
- If keys do nothing: "I pressed the up arrow and nothing happened.  What should
  the editor do when it receives that key?"
- If a file loads with an unexpected blank line at the end: "Opening a file that
  ends with a newline shows an extra blank line.  What causes that?"
- If the saved file looks wrong: "After saving, the file has [X] bytes but I
  expected [Y].  What determines the file size that gets written?"

---

## Exercise 8 — Add a Complex Command: BASIC Interpreter

### Prompting objective
Break a large feature into a sequence of prompts, each small enough to test
before the next one starts.

### Task
Add a `BASIC` command that loads and runs a line-numbered BASIC program stored
as a text file on the FAT12 disk.  The interpreter should support variables,
arithmetic, control flow, and basic I/O — enough to run simple programs.

### Starting prompt

> "Add a rudimentary BASIC interpreter where the command BASIC can be used to
> run a given file."

### Hints
Like the kernel, the interpreter benefits from plan mode before any code is
written.  Ask Claude to design the expression evaluator and statement dispatcher
first: "Think through how to structure a line-numbered BASIC interpreter before
writing any code — expression parsing, statement dispatch, and the FOR/GOSUB
stacks."  Agree on the structure, then proceed step by step.

An interpreter is too large to produce and verify in one go.  Ask for it in
four steps, testing after each one before moving to the next:

1. "Start with loading and listing the program only — no execution yet."
2. "Add execution: PRINT, LET, GOTO, and END."
3. "Add IF/THEN, FOR/NEXT, GOSUB, and RETURN."
4. "Add INPUT and built-in functions: INT, RND, LEN, CHR$, STR$, LEFT$, RIGHT$,
   MID$."

> **Side note — let Claude write the test programs.**  After each step, rather
> than writing a test program yourself, ask Claude: "Write a short BASIC program
> that tests what was just added."  It knows the dialect it just implemented and
> will produce something that exercises the new features directly.  You just run
> it and observe.

After step 2, write and run this program to verify before continuing:

```
10 PRINT "hello"
20 GOTO 40
30 PRINT "skipped"
40 END
```

After step 3, verify a FOR loop counts correctly.  After step 4, run the number
guessing game from the README.

Use the review prompt after step 3:

> "Review the control flow implementation.  What are the most likely problems
> with FOR/NEXT or GOSUB that I should test?"

When the interpreter is complete, ask Claude to update `README.md` with a BASIC
syntax summary and example programs, and update `REFERENCE.md` with the interpreter's
design — particularly the expression evaluator and the statement dispatch approach.

### Quality bar
The number guessing game from the README should run to completion with no
incorrect jumps or loop counts.

### Iteration cues
- If the program stops early or jumps to the wrong line: "I ran [program] and it
  went to line [X] instead of [Y].  What should GOTO/IF/FOR do in this case?"
- If a loop runs the wrong number of times: "FOR I = 1 TO 3 runs [N] times.
  How many times should it run, and what decides when it stops?"
- If a built-in function gives a wrong result: "I called [function] with [input]
  and got [output].  I expected [expected]."

---

## Exercise 9 — Add Subdirectory Support

### Prompting objective
Ask Claude to find the minimal change before making one that touches everything.

### Task
Add `MKDIR`, `RMDIR`, and `CD` so the shell supports a full directory tree, with
all existing file commands continuing to work without modification.

### Starting prompt

> "Add MKDIR, RMDIR, and CD commands to create, delete, and navigate folders."

### Hints
Before asking for the implementation, ask one design question:

> "What is the minimal change needed so that all existing file commands work in
> subdirectories automatically, without modifying each one individually?"

A good answer changes one place in the disk layer rather than every command.
If Claude proposes modifying DIR, TYPE, COPY individually, push back: "Is there
a way to make the directory layer aware of the current directory instead?"

Once you have the design confirmed, ask for the implementation:

> "Implement it.  The shell prompt should show the current path."

After this exercise, ask Claude to update both `README.md` (with the new commands
and a subdirectory example) and `REFERENCE.md` (with how the current directory is
tracked and how the disk layer was changed).

### Quality bar
Run through this sequence manually: create a directory, enter it, create a file,
go back up, try to remove the directory (should fail — not empty), delete the
file, remove the directory (should succeed).

### Iteration cues
- If the prompt shows the wrong path after `CD ..`: "After CD .. the prompt shows
  [X].  What should it show?"
- If file commands stop working after CD: "DIR shows no files after I change
  directory.  Is the new directory being searched, or is it still looking at the
  previous one?"

---

## Exercise 10 — Commission a Test Suite

### Prompting objective
Ask for tests that reveal bugs, not just tests that confirm things work.

### Task
Write tests for the FAT12 layer, the command processor, the editor, and the
BASIC interpreter.

### Starting prompt

> "Review the [component] code and write a set of test cases to validate that
> it is functionally correct."

### Hints
"Write tests" tends to produce tests that only confirm the happy path.  Add:

- "Focus on edge cases and error conditions rather than the normal case."

For the command processor, ask about isolation before the tests:

> "The tests should not touch any real file on the host machine.  What is the
> simplest way to run them against a fake disk in memory?"

After getting the test suite, use the review prompt:

> "Review the tests you just wrote.  Which ones would still pass even if the
> function being tested was broken?"

Anything that passes with a broken implementation is not testing anything useful —
ask Claude to replace it.

Update `REFERENCE.md` after this exercise to note how to run the tests and what each
test file covers — this is especially useful when returning to the project after a break.

### Quality bar
Build and run with `-fsanitize=address,undefined`.  Every test that passes should
fail if you deliberately break the thing it tests.

### Iteration cues
- If there are fewer than five tests per component, ask: "What error conditions
  can this code encounter?  Add one test for each."
- If a sanitizer error appears, paste the full output and ask: "What does this
  sanitizer error mean, and what is the fix?"

---

## The Session Pattern

```
1. Context first    Read README.md, REFERENCE.md and the translation reference.
2. One thing        One task or question per prompt.
3. Constraints      State what must not happen — Claude will respect them.
4. Review           Use the review prompt before running large results.
5. Test             Run the code and observe; describe what you see.
6. Iterate          Describe the observed symptom; ask for the cause.
7. Document         Ask Claude to update README.md and REFERENCE.md.
```

### When to change approach

| What you observe | What to do |
|---|---|
| Claude changes things you did not ask about | "Why did you change [X]? Is that needed for [Y]?" |
| Same problem after two fix attempts | "Explain what the code is supposed to do here, step by step" |
| Output is too large to know where to start | Use the review prompt first, then test the areas it flags |
| Unsure whether the result is correct | "What would a test that catches a bug here look like?" |
| Starting a new session on an existing project | Paste REFERENCE.md or ask Claude to read it — either way, do it first |

---

## Prompts to Avoid

| Avoid | Because | Use instead |
|---|---|---|
| Asking for everything at once | Too large to test meaningfully | One layer or feature at a time |
| "Fix this" with no observation | Claude guesses at the cause | Describe what you ran and what you saw |
| "Write tests for X" alone | Produces happy-path tests only | Add "focus on edge cases and error conditions" |
| Two unrelated tasks in one prompt | Hard to test separately | One topic per prompt |
| Accepting output without reviewing | Misses predictable problems | Use the review prompt on any large result |
| Starting a new session without context | Claude starts from scratch | Always open with REFERENCE.md |

---

