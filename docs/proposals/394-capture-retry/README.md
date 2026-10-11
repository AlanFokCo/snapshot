<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# SNEP-394: Retry mechanism for capture failures

<!-- toc -->
- [Summary](#summary)
- [Motivation](#motivation)
  - [Goals](#goals)
  - [Non-Goals](#non-goals)
- [Proposal](#proposal)
  - [User Stories](#user-stories)
    - [Story 1: a concurrent capture is holding the node's staging memory](#story-1-a-concurrent-capture-is-holding-the-nodes-staging-memory)
    - [Story 2: the dump fails after the workload is already suspended](#story-2-the-dump-fails-after-the-workload-is-already-suspended)
    - [Story 3: the workload is too large to capture on this node](#story-3-the-workload-is-too-large-to-capture-on-this-node)
  - [Limitations, Risks, and Mitigations](#limitations-risks-and-mitigations)
- [Design Details](#design-details)
  - [Deciding what to repeat](#deciding-what-to-repeat)
  - [Whether a failed dump can simply be retried](#whether-a-failed-dump-can-simply-be-retried)
  - [Reviving a suspended workload](#reviving-a-suspended-workload)
  - [Attempt lifecycle](#attempt-lifecycle)
  - [Failures that end the capture immediately](#failures-that-end-the-capture-immediately)
  - [Protecting the node agent](#protecting-the-node-agent)
  - [Storage cleanup](#storage-cleanup)
  - [API](#api)
  - [Security](#security)
  - [Configuration](#configuration)
  - [Performance and Scalability](#performance-and-scalability)
  - [Monitoring](#monitoring)
  - [Dependencies](#dependencies)
  - [Test Plan](#test-plan)
    - [Measured behaviour](#measured-behaviour)
  - [Graduation Criteria](#graduation-criteria)
- [Appendix](#appendix)
<!-- /toc -->

## Summary

A checkpoint capture that fails for a transient reason is retried in place,
against the workload that is already running, instead of being thrown away.
Each failure is recovered by the cheapest means that can resolve it, so a retry
repeats only the work that is no longer valid and never reloads a model. How
many times to retry is configurable cluster-wide and per capture. Failures a
retry cannot fix end the capture immediately, with a reason naming the cause.

## Motivation

A capture is expensive to reach. By the time the node agent dumps a container,
the workload has pulled its image, loaded its model into GPU memory, and passed
its readiness probe — minutes of work holding GPUs throughout. Today a single
transient error anywhere in that capture throws all of it away: the
`PodSnapshotContent` is marked `Failed`, the source process is killed, and
nothing retries. The caller has to notice and retrigger the capture by hand,
paying the full setup cost again.

The errors that trigger this are frequently not deterministic. PageBroker
staging can be momentarily exhausted by a concurrent capture on the same node
(#198). A CRIU dump can fail for reasons that do not recur. The overlay diff
step can fail mid-capture (#142). Storage I/O can fail transiently. In each
case a second attempt against the same workload would very likely succeed, and
would cost seconds rather than minutes.

A second problem sits on the same path. The node agent is one process per node
serving every capture *and* every restore on that node, and it has no panic
recovery: the capture runs in a bare goroutine, so one malformed work order can
terminate the agent and take every unrelated in-flight capture and restore with
it. Adding attempts and recovery logic to that path without first making it
survivable would multiply the exposure.

This proposal is the capture-side counterpart of the restore-side work in #244
(fallback to cold start) and #247 (restore retry). It implements the retry half
of #319.

### Goals

1. A capture that fails for a transient cause succeeds on a subsequent attempt
   without manual intervention, against the workload that is already running.
2. A retry repeats only work that is no longer valid, decided from the observed
   state of the workload.
3. A capture that cannot be recovered fails immediately, keeping its retries,
   and says why.
4. Retry limits are configurable, with a cluster-wide default an individual
   capture can override.
5. An operator can tell from the Kubernetes API why a capture failed and
   whether it was retried: each attempt reports its own cause, a capture that
   used up its retries reads differently from one that failed on the first try,
   and a failure needing operator action reads differently from one caused by
   the workload.
6. A single capture request cannot terminate the node agent, by panic or by
   exhausting the agent's memory.

### Non-Goals

1. **Restarting the source pod.** No second source Job, `PodSnapshot`, or
   `PodSnapshotContent` is created. Every recovery acts on the pod already named
   by `PodSnapshotContent.spec.source.podRef`.
2. **Keeping the source alive past a successful dump.** The dump still
   terminates the workload, so a capture whose data is lost after the dump
   succeeded is not recoverable.
3. **Surviving a node-agent restart.** An attempt in flight when the agent dies
   is not resumed; the work order fails through the existing content resync and
   capture-lease expiry. Keeping the agent alive is goal 6; resuming across its
   death is not.
4. **Telling the workload that no checkpoint is coming.** The other half of
   #319. It changes the workload contract rather than the agent's capture path.
5. **Moving publish retry into PageBroker.** The agent owns the retry; the
   broker's contract is unchanged.
6. **Restore-path retry or cold-start fallback** (#247, #244). Setting names are
   aligned with #247 so the two read as a pair, but the mechanisms are separate.

## Proposal

A capture runs four steps: **stage** a destination, **suspend** the workload's
GPU state, **dump** the process, and **store** the result. The dump terminates
the workload by design, so what a failure leaves behind depends entirely on
where it happened.

When an attempt fails, the agent reads the workload's actual state and takes
the cheapest recovery that fits:

| Workload state | Recovery |
| --- | --- |
| Running, untouched | discard the attempt and start another |
| Suspended, and can be dumped as it stands | dump again, leaving the GPU state alone |
| Suspended, but the failed attempt left it unusable | revive it, then suspend and dump again |
| Terminated, checkpoint staged | store again; no live workload is needed |
| Terminated, nothing staged | the capture fails |

The second row is the one that matters most. Suspending a workload's GPU state
is by far the expensive step, and a failed dump usually leaves that work done
and still valid — so the common case repeats only the dump. The third row is
the fallback for when the failed attempt left the workload in a state the dump
cannot start from; deciding between the two is
[the dumpability check](#whether-a-failed-dump-can-simply-be-retried).

Anything else ends the capture immediately with a reason naming the cause.
Retries continue while the configured limit allows, and a capture that uses up
its retries reports that distinctly from one that failed on the first try.

### User Stories

#### Story 1: a concurrent capture is holding the node's staging memory

A platform team captures a vLLM workload. Another capture on the same node is
storing its result and holds the node's staging, so the broker refuses to stage
this one before allocating anything — the workload is untouched and still
serving.

Today that is terminal and the team re-runs the job, reloading the model. With
this proposal the agent waits and stages again. The capture succeeds, having
cost a wait rather than a model load, and a warning event records the transient
failure.

#### Story 2: the dump fails after the workload is already suspended

The same capture gets further on a later run: the GPU state has been moved to
host memory and the dump then fails.

Today the workload is killed and the model is gone. With this proposal the
agent finds it alive and still dumpable and dumps again, skipping the expensive
suspend entirely.

#### Story 3: the workload is too large to capture on this node

A workload's checkpoint image is bigger than the node can stage. No amount of
waiting helps: there is no moment at which it fits.

The agent establishes this from its own size estimate and fails immediately
with a reason naming the shortfall, so the operator resizes staging or moves
the workload. Retries are not spent on it, and the node keeps serving everyone
else.

### Limitations, Risks, and Mitigations

**Reviving a suspended workload narrows a documented invariant.** The capture
path states today that a CUDA-locked source is never resumed. This narrows it
to: never resumed unless the agent has verified it can be. The danger is a
workload that comes back only partly and is then captured anyway. The agent
therefore reads each GPU process's state rather than inferring it, before and
after, and ends the capture if any of them does not return to running — the
current outcome, rather than a corrupt artifact.

**CRIU's failure-path behaviour is not a stable contract.** Dumping again after
a failed dump depends on CRIU leaving the task tree alive and releasing it.
This was measured and holds (see [Test Plan](#test-plan)), but the agent builds
CRIU from a moving upstream branch, so it could change with no version change
in this repository. The dumpability check mitigates it: a tree that does not
present as cleanly suspended is revived or fails, so a CRIU regression degrades
retry rather than breaking capture.

**Retry extends how long a workload holds its GPUs.** A capture that retries
occupies the source pod, its GPUs, and node staging for longer than one that
fails immediately. Bounded by the configured retry limit and by the capture
timeout, as described under [Attempt lifecycle](#attempt-lifecycle).

**Every attempt targets one destination.** Retrying in place means a new
attempt cannot stage until the previous one has fully released, or it conflicts
on that destination and leaves the previous staging occupying node memory.
Release is therefore a confirmed step, not best-effort.

## Design Details

Retry lives entirely within the node agent, inside the handling of one
`PodSnapshotContent`, under the in-flight guard and capture lease the agent
already holds. No controller above the agent participates.

### Deciding what to repeat

The agent reads state rather than inferring it from how the attempt failed. Two
inputs: **which step failed**, which it knows because it controls the sequence
and which tells it whether staged data might exist; and **a probe** of the
workload. The probe decides.

The agent's kill of the source immediately after a dump failure is removed.
Today it fires within milliseconds, destroying the state every recovery depends
on; what happens to the workload is now the probe's decision.

### Whether a failed dump can simply be retried

By the time a dump runs, the suspend has already moved the workload's GPU
memory to host memory — the expensive step. So after a dump fails the agent
needs the answer to one question: **can CRIU dump this process again as it
stands?** If it can, the retry costs only the dump. If it cannot, the workload
has to be put back to running and suspended afresh, paying that GPU transfer a
second time.

The answer can be no because dumping is invasive. To dump a process tree CRIU
`ptrace`-attaches to every task in it — *seizing* it — and injects a small blob
of code, the *parasite*, which runs inside the process to read out its own
memory. CRIU undoes both when it finishes, on success and on failure alike. But
a run that is interrupted, or that fails in a way which skips its cleanup, can
leave tasks still attached or still stopped, and **CRIU cannot seize a process
that is already seized**. Such a process must be reset before it can be dumped
again.

The agent therefore checks that nothing from the failed attempt is still
attached, and that the tree is the one it suspended:

- **No task is traced or stopped** — `TracerPid` is `0` and no task is in state
  `T`. This is the direct evidence that CRIU released the process.
- **No task is in uninterruptible sleep** (state `D`) — such a task is blocked
  inside a driver call and cannot be seized reliably.
- **The tree is unchanged** — every process recorded at suspend is present and
  its thread count still matches, so nothing exited or forked after CRIU let go.
- **The GPU state is uniform** — every GPU process reads `checkpointed`. A
  suspend that failed partway leaves some processes merely `locked`, and that
  tree is not the one the dump was meant to capture.

When all four hold, the process is exactly what it was before CRIU touched it,
and the agent dumps again without disturbing the GPU state. Measurement shows
this is the normal outcome: after a failed dump the workload read task state
`S`, `TracerPid` `0`, an unchanged thread count and `checkpointed` GPU state —
indistinguishable from one that had only ever been suspended
([Test Plan](#test-plan)).

When any of them does not hold, the workload cannot be dumped as it stands, but
it is not lost. Reviving it and suspending again produces a clean starting
point, and this is the only path that pays the GPU transfer twice.

### Reviving a suspended workload

GPU checkpointing is a reversible state machine: `lock` blocks new GPU work,
`checkpoint` moves GPU memory to host memory, and `restore` and `unlock` undo
them. Reviving in place needs no device remapping, because the workload is
being handed back the GPUs it still holds.

Revival must dispatch **per process**, because a failed suspend leaves a mixed
tree — the agent locks every process and then checkpoints every process, so a
failure in between leaves some `checkpointed` and some only `locked`:

| Probed state | Action |
| --- | --- |
| `running` | none |
| `locked` | `unlock` |
| `checkpointed` | `restore`, then `unlock` |
| `failed`, or process absent | end the capture |

Every GPU process must read `running` afterwards. The restore path's existing
restore-and-unlock helper cannot be reused: it restores every process and
returns on the first error, so against a mixed tree it fails *and* leaves every
process locked — worse than before the attempt (see [Test Plan](#test-plan)).

### Attempt lifecycle

Each attempt is a self-contained function invoked from a loop, so an attempt's
cleanup completes at the end of *that attempt* rather than accumulating.

Three properties are load-bearing:

- **A store retry must not release its staged data.** The deferred release that
  runs on other failure paths would destroy the only copy of a successful dump,
  so release is conditional on which recovery was chosen.
- **An attempt that re-stages must confirm release first.** The agent proceeds
  only once the previous transaction is terminal and its transfers have drained.
- **The lease is re-checked between attempts.** A lost lease means another
  holder may own the work order; retrying under one ends the capture.

Attempts are spaced by exponential backoff whose initial delay and ceiling are
hard-coded constants. The total time a retrying capture can consume is already
bounded by the capture timeout the workload sets.

### Failures that end the capture immediately

A probe that cannot account for the workload is the primary rule. Alongside it:

- **Identity and configuration errors** — a stale pod reference, a conflicting
  content binding, a missing content UID, an invalid target container or
  destination. A further attempt reproduces them.
- **A lost or cancelled lease**, and **an exceeded capture timeout**.
- **A workload that failed on its own**, before the capture.
- **Environment incompatibility** — an unsupported kernel or CRIU feature, a
  missing seccomp profile, a driver the helper rejects. Deterministic for this
  node, and an in-place retry stays on this node.
- **Destination store exhaustion**, and **an image too large for the node's
  staging** ([Protecting the node agent](#protecting-the-node-agent)).
- **Staged data lost after a successful dump** — the workload is already gone,
  so there is nothing to dump again.
- **Broker rejections a retry cannot change** — a malformed request or a
  destination conflict.

Transient broker conditions are retried: staging refused under contention,
storage errors, and internal errors. The protocol sanctions releasing and
retrying with a new transaction for the latter two.

### Protecting the node agent

Two ways a single capture can terminate the agent that serves every capture and
restore on its node.

**Panics.** The capture runs in a bare goroutine with no recovery. Each
goroutine boundary on the capture and restore paths gains a recovery that turns
the panic into a terminal failure for that one work order, releasing the guard
and lease, and lets the agent continue. Values read from a `PodSnapshotContent`
or a pod are treated as untrusted, so a violated invariant becomes a terminal
status with a named reason rather than a crash.

**Memory exhaustion.** CRIU writes the staged image from the agent's own
container, and memory-backed staging pages are charged to the writer, so a
checkpoint larger than the agent's memory limit terminates the agent with no
error at all. Staging is a memory-backed `emptyDir` that today carries no
`sizeLimit`, so it can grow until the agent's cgroup kills it.

The containment is a bound the kernel enforces: a `sizeLimit` makes the volume
fill first, so the write fails with `ENOSPC` and the agent handles an ordinary
capture failure. It must leave room rather than merely match, because staging
pages and the agent's own allocations share one cgroup — the invariant is
**staging `sizeLimit` ≤ agent memory limit − reserve**, where the reserve
covers peak non-staging use. Sizing that reserve, and testing that a staging
write returns `ENOSPC` before the agent reaches its limit, are part of the work.

A pre-staging size estimate is an optimisation on top, not the safeguard. Image
size is bounded only from *below* by the workload's resident memory plus its
GPU state, and a lower bound can prove an image will not fit, never that it
will — so it is used in that direction only, to fail before a model's worth of
time is spent.

**Telling a full node from an image that will never fit.** Both surface the
same broker error, and confusing them would spend the retry limit on an image
that cannot succeed. The failing step separates them: refusal before any bytes
are written means the node is busy and nothing about this image is implicated,
so it is retryable; `ENOSPC` once the dump is writing proves the image exceeds
capacity, so it ends the capture with the measured shortfall.

### Storage cleanup

Every attempt shares one content UID and therefore one destination, which makes
cleanup a correctness prerequisite rather than a capacity concern. Staging is
released per attempt. On a terminal failure the work order's artifacts are
reclaimed by the existing `PodSnapshotContent` cleanup finalizer and the
operator's maintenance workqueue (#349, #390).

The artifact destination must stay empty until a capture has genuinely
committed, because the agent treats an existing artifact directory with a
matching manifest as a committed capture. Publication must therefore be atomic
or fully reverted on release — a requirement on the storage path, verified by
the test plan.

### API

The retry limit is configurable cluster-wide and per capture, so two surfaces
change.

**Cluster-wide default** — a retry setting in the agent configuration, rendered
by the Helm chart ([Configuration](#configuration)).

**Per-capture value** — an optional field on `PodSnapshot.spec`, the object that
represents a capture request. It follows the path every other capture parameter
already takes: a caller creating a `PodSnapshot` sets it directly; `SnapshotJob`
exposes it on `spec.podSnapshotTemplate` alongside `targetContainers` and copies
it into the `PodSnapshot` it creates; and the `PodSnapshotReconciler` copies it
into `PodSnapshotContent.spec`, where the agent already reads the work order.
Both specs stay immutable, since the value is set at creation. An out-of-range
value is rejected by CRD validation at admission.

One choice for review: whether an operator should be able to cap what a caller
may request.

### Security

Revival uses capabilities the agent already exercises on the capture and
restore paths, and artifact paths, ownership, and cleanup are unchanged.

The proposal's main security effect is an improvement to **tenant isolation**.
The node agent is shared by every workload on its node, and today a single
capture request can terminate it by panic or by memory exhaustion, denying
service to unrelated tenants. Both are closed, so the blast radius of one
tenant's bad request becomes one failed capture.

Two smaller considerations:

- **Failure detail reaching the API.** Panic recovery and failure
  classification produce diagnostics that could include internal paths or stack
  traces. Conditions and events carry a bounded message; full traces stay in
  agent logs, which are already operator-scoped.
- **Resource holding, and who may extend it.** Because the retry limit is
  settable per capture, a caller can lengthen its own hold on shared GPUs and
  staging. Whether operators should be able to cap that is the open question in
  [API](#api).

### Configuration

**The retry limit is the only setting** — how many further attempts may follow
the first.

The agent reads the per-capture value; absent, it uses the cluster default.
Absent is distinct from zero: zero means the caller explicitly wants no
retries, absent means it expressed no preference. The effective limit is
resolved once when the capture begins and logged with it.

The cluster default is **0**, so retry is opt-in and an upgrade changes no
existing capture's behaviour. This matches the default #247 states for restore
retry, and the setting's name should be agreed with it.

The node-agent protections are independent of this default: panic recovery and
the fail-fast size check are unconditional.

### Performance and Scalability

A successful capture is unaffected: the probe and the recovery decision run
only after a failure.

A failing capture becomes slower by design, and its workload and GPUs stay
occupied for that duration — the explicit trade against re-reaching a ready
workload from scratch.

Dumping again without reviving is what keeps the common retry cheap. Suspending
and reviving GPU state each move the workload's full GPU footprint across the
PCIe bus, so avoiding both saves roughly twice that transfer. The saving scales
with the workload's GPU footprint, not with the number of retries.

Node memory is the scarce resource. Staging is memory-backed and shared by both
containers in the agent pod, so a retry that failed to release its
predecessor's staging would double the demand; release is therefore confirmed,
and the size check keeps a capture that cannot fit from being attempted.

### Monitoring

- **Events** on the source pod for each failed attempt, naming the recovery
  taken and the cause.
- **A distinct terminal reason for exhausting the retry limit**, so "failed
  after exhausting its retries" reads differently from "failed once".
- **Distinct reasons separating storage conditions**: staging refused under
  contention (retried), destination store exhausted, and image too large for
  the node (both operator action).
- **Structured logs** per attempt, including the effective retry limit and the
  CRIU version in use — with a moving CRIU reference, "which CRIU produced this
  failure" is otherwise unanswerable from a bug report.

Metrics follow as their own change: the agent has no metrics client, endpoint,
or scrape configuration today. A non-terminal condition on `PodSnapshotContent`
exposing attempts in status would be additive and safe, and can follow if
operators ask for it.

### Dependencies

In the tree already: the GPU checkpoint and restore helper actions used for
suspend, probing, and revival; a CRIU version query; process-tree enumeration;
the pod-event mechanism; the injection seam used to simulate capture failures
in tests; and the artifact cleanup machinery (#349, #390).

One qualification: **typed PageBroker failure codes.** Classifying broker
failures by code rather than text needs an accessor on the client's failure
type, which #395 introduces as part of a larger protocol change. A minimal
accessor can be added here instead, so this proposal does not depend on that
work landing.

This proposal assumes the PageBroker data path (#251).

### Test Plan

**Unit.** The recovery decision is a pure function of failing step and probe
result, table-tested across every combination, including each dumpability
signal failing on its own. Immediate-failure classification is tested per
reason. Retry bounds are tested for limit expiry and for correct reporting of
exhaustion. Limit resolution is tested for precedence, for absent versus
explicit zero, and for rejection of an invalid value. Release behaviour is
tested for the property that a store retry retains its staged data and a
re-stage does not proceed before confirmed release. Panic recovery is tested by
panicking inside the existing capture injection seam and asserting the agent
survives with the work order terminally failed.

**Integration.** Agent controller tests cover a capture that fails once and
succeeds on retry, one that exhausts its retries, and one hitting an
immediate-failure classification, each asserting the resulting conditions and
events. That the artifact destination stays empty until a genuine commit is
asserted directly, since same-destination retry depends on it.

#### Measured behaviour

Measured on a single-node GPU cluster (4× T4, driver 595.71.05, kernel
7.0.0-1014-aws, CRIU 4.2.1) against a workload holding 256 MiB on-device and
re-checksumming it on the GPU every beat, so a recovered workload has to launch
kernels and return correct data rather than merely exist.

| Question | Result |
| --- | --- |
| Does a failed dump leave the tree alive? | Yes — before and after the freeze. CRIU unfreezes and unseizes as part of failing. |
| Does a failed dump leave anything attached? | No. Task state `S`, `TracerPid` 0, thread count unchanged, GPU state `checkpointed` — identical to a clean suspend. |
| Does the agent kill the source after a failure? | Yes, within 12 ms — which is why that kill is removed. |
| Can a suspended workload be revived? | Yes. Full cycle repeated with **0 checksum mismatches** and GPU memory retained. |
| Can a revived workload be captured again? | Yes — it re-enters the suspend state machine cleanly. |
| Does per-process dispatch matter? | Yes. Restoring a `locked` process returns `CUDA_ERROR_ILLEGAL_STATE`; the existing helper then returns before unlocking, leaving every process locked. |
| Does a staging `sizeLimit` contain an oversized capture? | Yes. The dump failed with `ENOSPC` mid-write and the agent survived with 0 restarts. |
| What does suspend/revive cost? | ~300 ms suspend, ~220 ms revive per 256 MiB — roughly 2 GB/s each way. |

**Not yet shown:** a successful capture *after* a failed dump through the
production agent, which requires the kill above to be removed — the change this
proposal makes. End-to-end coverage therefore closes with induced transient
failures for each recovery against the framework workloads already used by the
suite, asserting the capture succeeds on retry and that a retried capture
produces an artifact indistinguishable from a first-attempt one.

### Graduation Criteria

**Alpha.** Retry is available, defaulting to zero retries, configurable
cluster-wide and per capture, with unit and integration coverage for the
recoveries that do not require reviving a suspended workload. The node-agent
protections are in place and unconditional. Failure classification distinguishes
retryable from immediate causes, and storage conditions are reported distinctly.

**Beta.** Revival is implemented and enabled. End-to-end coverage demonstrates
each recovery against the framework workloads, including a capture that
succeeds after a failed dump, and a retried capture is shown to restore
correctly.

**GA.** Field evidence that the default configuration is right, that revival
succeeds at a rate justifying its complexity, and that no capture has been
observed to terminate a node agent. Configuration naming is settled jointly
with restore retry (#247).

## Appendix

- #319 — the feature request this proposal implements the retry half of.
- #247, #244 — the restore-side counterparts.
- #237, #251 — PageBroker integration and its adoption as the data path.
- #349, #390 — the artifact cleanup machinery this proposal reuses.
