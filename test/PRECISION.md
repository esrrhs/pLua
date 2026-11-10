# Precision design and validation notes

## Why boundary snapshots instead of vLua's handler

The read-only reference `vLua/src/vlua.cpp` walks `L->ci` / `CallInfo::previous`
inside SIGPROF, retains `CIST_LUA` frames, and uses VM-layout-specific extraction
and a fault guard. pLua supports the public Lua API across versions; porting that
interrupted mutable-stack walk (or calling `lua_getstack` in a signal) would not
meet its signal-safety contract.

The previous pLua handler both iterated mutable `gSessions` and marked *every*
running state with one boolean. A long C call collapsed many delivered ticks to
one; that one sample could be taken inside unrelated Lua after the return.

The new handler increments one statically lock-free unsigned atomic. Normal
hooks drain ticks to the previous stable snapshot, then capture the new stack.
Call/return hooks surround C calls and C->Lua callbacks. A return snapshot omits
the returning frame, since Lua's return hook runs before popping it. Tail-call
hooks use the replacement stack. Owner selection is single-threaded; the handler
never dereferences it. Timer intervals are shared consistently across sessions.
Stop disables the timer before its final drain. Limits cap weights, not batches.
The CPU metadata is written after the backend is known.

Coroutines share a registry, so anchors must be per-thread and retained threads
must be rooted. Explicit teardown nulls the old anchor before removing it, so a
later finalizer cannot destroy a restarted session. A distinct registry key and
metatable name avoid accidentally using the anchors table as a metatable.

## Deterministic versus statistical tests

`precision_test` is the CI gate. It uses synchronous `raise(SIGPROF)` to test
*delivered* ticks with a one-second natural timer, without elapsed-time criteria:

- C batch: 37/37 ticks to `hot_caller`, zero to later `unrelated` Lua work.
- Lua->C->Lua: 11+13 ticks to the outer Lua caller, 23 to the Lua callback.
- Tail call: 17 to the live target, no invented erased caller.
- Direct C stop with no intervening hook: 31/31 pending ticks retained/flushed.
- Repeated unenrolled coroutine resumes: 19+29 excluded, 7+5 parent ticks kept.
- Explicit child session: 41 child ticks, 3 parent ticks, independent files.
- Pre-existing unhooked coroutine: documented limitation is exposed, not hidden:
  its 17 ticks are charged to the enclosing resume caller.
- Independent states: idle host ticks excluded, state totals 11 and 22, not a
  broadcast of all ticks to both. Conflicting timer starts must leave the first
  session functional (13/13 ticks after rejection).
- Active coroutine global close, restart/old-anchor GC, file-open failure,
  weighted sample-limit cap, and bounded instruction-count output.

`precision_test --experiment` is deliberately separate from CTest. It burns
600ms of process CPU inside one C function and then runs unrelated Lua. It
reports count/timing bias instead of imposing flaky statistical thresholds.

Observed locally on Linux / Lua 5.4.8, comparing the same harness with the
unmodified `6ec5e03` sampler (built from `git show`, no branch changes):

| Sampler | Three repeated 600ms C runs: total / hot / unrelated |
|---|---|
| `6ec5e03` | 1 / 0 / 1; 1 / 0 / 1; 1 / 0 / 1 |
| Boundary snapshots | 59 / 59 / 0; 61 / 61 / 0; 63 / 63 / 0 |

A separate real-CLI validation run during other build/test work produced
68 / 68 / 0 (680ms estimated). Against a nominal 60 ticks, the observations
range from -1 to +8 ticks: attribution improves decisively, but the timer and
snapshot method are not an exact CPU clock. Do not treat these observations as a
universal accuracy bound or a statistical benchmark.

## Export validation

Go tests independently decode profile.proto fields and check leaf-first stacks,
nonzero function/location IDs, string index zero, sample/period types and weights.
They reject malformed/truncated input, unknown metadata, missing frame IDs,
duplicate string IDs and overflowing CPU weights. Legacy and flame formats are
retained. Real `go tool pprof -top` accepts CPU and count fixtures: 7 samples at
10ms yields 70ms flat at `hot_leaf`; count reports 7 samples, not nanoseconds.
A real C workload converted through `bin/plua -proto` was also accepted (680ms
flat at `hot_caller`); the count workload reports 2000 samples at `count_hot`.

Memory/v0/v1 cannot reliably identify CPU/backend/period and are explicitly
rejected by `-proto`, rather than mislabeled. Their legacy/flame output remains
available. No native C unwinding, line-level sampling, rotation, new dependency,
or timestamp/duration inference is introduced.

## Remaining limits

See both READMEs: one OS thread, process-CPU contamination from other native
threads, kernel signal coalescing, stale intervals/error unwinds/continuations,
pre-existing unhooked coroutines and uninstrumented state transitions, erased
tail callers, maximum 64 frames, and workload-dependent call/return-hook overhead.
Count backend cannot measure C CPU time. SIGPROF/ITIMER_PROF remain exclusive to
pLua. Local Linux builds/tests and ASAN+UBSAN precision/lifecycle checks were run;
Windows/macOS and Lua 5.2/5.3 runtime behavior still need their platform CI.
