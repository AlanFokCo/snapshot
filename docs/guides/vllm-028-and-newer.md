# vLLM 0.28+ compatibility notes

These notes supplement the [existing vLLM guide](vllm.md). They do not change
its application, deployment manifests, or pinned 0.27.1 image. For a validated
version newer than 0.28, our tested runtime was **vLLM 0.30.0**, using the
official Ubuntu 24.04 image:

```text
vllm/vllm-openai:v0.30.0-ubuntu2404@sha256:439c19d48db36401abc914b9842d060fe610a54bc3ac29bb02505f0b69af1baf
```

The previous guide's warning about 0.28 must not be read as a blanket
restriction on every newer version: 0.30.0 checkpoint, restore, and inference
passed in the configurations below. **0.28 and 0.29 themselves were not
rerun**, and the result does not establish compatibility with every later
version or model.

## What was validated

| Workload | Snapshot | Setup | Result |
| --- | --- | --- | --- |
| vLLM 0.30.0, one GPU | 0.1.0 | Application defaults worker multiprocessing to `spawn` before importing vLLM | Checkpoint, restore, and HTTP inference passed |
| vLLM 0.30.0, one GPU | 0.1.0 | Original application launched under `cuda-checkpoint --launch-job` | Checkpoint, restore, and HTTP inference passed |
| vLLM 0.30.0, TP=2 | 0.2.0-rc.2 | SnapshotJob with shared-memory annotation and automatic cuInterpose launch setup | Checkpoint, restore, and HTTP inference passed |

The TP=2 validation used Qwen/Qwen3-0.6B on two NVIDIA B200 GPUs on one node,
with driver 595.58.03 and VAST RWX checkpoint storage. Source and restore
used the same immutable runtime image. This is release-candidate validation
of that workload and topology, not blanket qualification of multi-GPU
workloads or cross-node/cross-Pod parallelism.

## Single-GPU, multi-process applications

Multiple worker processes do not necessarily mean multiple GPUs. For the
single-GPU vLLM path, initialize the process start method before importing
vLLM:

```python
os.environ.setdefault("VLLM_WORKER_MULTIPROC_METHOD", "spawn")
```

The current guide's `app.py` already does this. In our 0.30.0 test, the
unwrapped application without it failed capture on an open `/dev/nvidiactl`
descriptor; the `spawn` variant passed. Native CUDA job wrapping also passed
the single-GPU test without that setting. This does not mean `spawn` alone
handles shared CUDA memory in a tensor-parallel workload.

## Multi-GPU, multi-process applications

For the validated shared-memory path, Snapshot 0.2.0-rc.2 supports this opt-in
on the SnapshotJob's **Pod template**:

```yaml
spec:
  podTemplate:
    metadata:
      annotations:
        nvidia.com/cuda-shared-memory-support: enabled
```

SnapshotJob installs the matching cuInterpose libraries and launcher and
prepends the launcher to the application command. Do not manually prepend
another launcher when using this automatic setup. An annotation on the
SnapshotJob's top-level metadata does not enable the source setup.

The workload must separately request two GPUs and configure vLLM with
`tensor_parallel_size=2`. Merely increasing `nvidia.com/gpu` does not change
the engine configuration. The existing single-GPU `app.py` and Deployment do
not enable TP=2 automatically. Source and restore need matching GPU counts,
application/model mounts, and sufficient shared memory; the test mounted a
4Gi memory-backed volume at `/dev/shm`.

An ordinary Pod or Deployment does not get SnapshotJob's automatic installer
and command shaping. Using the interposer there requires manually supplying
the matching libraries, mounting them read-only at `/tmp/snapshot-cuda`, and
starting the application with:

```text
/tmp/snapshot-cuda/cuinterpose-launch --library /tmp/snapshot-cuda/libcuinterpose.so -- <workload-command>
```

The annotation is not an application lifecycle hook. The application must
still finish GPU work, park the participant group before capture, and resume
only after `restore-complete`. Use compatible, matching Snapshot payloads;
library identity is checked during capture and restore. Revalidate changes
to the model, driver, GPU count, placement, or shared-memory behavior.

## Native CUDA job wrapping without the annotation

Native `cuda-checkpoint --launch-job` and cuInterpose are different paths.
SnapshotJob does **not** automatically add native CUDA job wrapping. For the
native alternative, launch the workload under the wrapper before it creates
CUDA state and worker processes, persist the driver-created job file at
`/snapshot-control/cuda-checkpoint-job`, and point `CUDA_CHECKPOINT_JOB_FILE`
to that stable path. The workload image must contain the required binary;
source and restore must provide the required binary layout at the same path.

NVIDIA documents native `cuIpcGetMemHandle` checkpoint support as a **driver
610 feature**, and [recommends driver 610 or newer for job wrapping](https://github.com/NVIDIA/cuda-checkpoint#610-features).
The wrapper can expose a job file on an older driver without supplying that
native IPC support. Our driver-595 single-GPU wrapper result does not qualify
native TP=2: that alternative was not validated in this run. The tested TP=2
path on driver 595.58.03 used the annotation-enabled interposer instead.

## Standalone installation and evidence of a warm restore

Standalone Snapshot does not require Dynamo. Helm installs the Snapshot
infrastructure; it does not configure arbitrary vLLM workloads or implement
their sleep/wake lifecycle. All paths must satisfy the
[workload contract](../reference/workload-contract.md).

Disable Datadog GPU monitoring or other external CUDA interception for the
workload before capture and through restore and inference. The validated
cuInterpose path is an explicitly opted-in Snapshot component, not permission
to stack unrelated interceptors.

The live TP=2 test checked the generated launcher, running workers and two
visible GPUs, then waited for SnapshotJob `Captured=True`, `Completed=True`,
and `Failed=False`. It restored into an inert `sleep infinity` placeholder,
verified `nvidia.com/Restored=True`, inspected the restored PID and workers,
and issued a real HTTP `/generate` request. The response was HTTP 200 with
non-empty generated text; the restore logs showed wake/resume rather than a
second model initialization. A Running pod or a successful native CUDA
restore alone is not enough to establish restored inference.

Recorded on October 12, 2026. This evidence is a functional check, not a
throughput or latency benchmark. The general [limitations](../limitations.md)
still apply outside the specifically validated configuration.
