<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Run a multi-GPU example

These recipes checkpoint and restore an eight-GPU workload on one node.

## Prerequisites

- [Snapshot installed](../operations/install.md) on nodes that meet the
  [driver requirements](../../README.md#prerequisites).
- Eight GPUs on one node, with the CPU and memory the manifests request. The
  manifests select B200 nodes. Change `nodeSelector` for other GPUs.
- A ReadWriteMany storage class for the [model cache](model-cache-pvc.yaml).
- For Deployment capture, `envsubst` (`gettext-base` on Debian and Ubuntu).

## Recipes

| Model | Engine | Deployment | SnapshotJob | Restore |
| --- | --- | --- | --- | --- |
| GLM 5.3 NVFP4 | vLLM | [Manifest](vllm/capture/glm-5.3.yaml) | [Manifest](vllm/capture/glm-5.3-snapshotjob.yaml) | [Manifest](vllm/restore/8-gpu.yaml) |
| GLM 5.3 NVFP4 | SGLang | [Manifest](sglang/capture/glm-5.3.yaml) | [Manifest](sglang/capture/glm-5.3-snapshotjob.yaml) | [Manifest](sglang/restore/8-gpu.yaml) |
| GLM 5.3 NVFP4 | TensorRT-LLM | [Manifest](tensorrt-llm/capture/glm-5.3.yaml) | [Manifest](tensorrt-llm/capture/glm-5.3-snapshotjob.yaml) | [Manifest](tensorrt-llm/restore/8-gpu.yaml) |
| DeepSeek V4.1 Flash | vLLM | [Manifest](vllm/capture/deepseek-v4.1-flash.yaml) | [Manifest](vllm/capture/deepseek-v4.1-flash-snapshotjob.yaml) | [Manifest](vllm/restore/8-gpu.yaml) |
| DeepSeek V4.1 Flash | SGLang | [Manifest](sglang/capture/deepseek-v4.1-flash.yaml) | [Manifest](sglang/capture/deepseek-v4.1-flash-snapshotjob.yaml) | [Manifest](sglang/restore/8-gpu.yaml) |

The steps below use vLLM with GLM 5.3. For another recipe, use its engine's
`app.py`, ConfigMap name, and manifests from the table.

## 1. Prepare

Run from the repository root:

```bash
export SNAPSHOT_NAMESPACE=<namespace>

kubectl apply --namespace "$SNAPSHOT_NAMESPACE" \
  --filename docs/guides/model-cache-pvc.yaml

kubectl create configmap vllm-app --namespace "$SNAPSHOT_NAMESPACE" \
  --from-file=app.py=docs/guides/vllm/app.py \
  --dry-run=client -o yaml | kubectl apply --filename -
```

## 2. Capture

Use either a SnapshotJob or a Deployment.

### With a SnapshotJob

```bash
kubectl apply --namespace "$SNAPSHOT_NAMESPACE" \
  --filename docs/guides/vllm/capture/glm-5.3-snapshotjob.yaml

kubectl wait --namespace "$SNAPSHOT_NAMESPACE" \
  --for=condition=Completed snapshotjob/vllm-glm-5-3-snapshot --timeout=120m
```

### With a Deployment

Set `SNAPSHOT_AGENT_IMAGE` to the image of the installed Snapshot agent:

```bash
export SNAPSHOT_AGENT_IMAGE=<registry>/snapshot/agent@sha256:<digest>

envsubst '${SNAPSHOT_AGENT_IMAGE}' < docs/guides/vllm/capture/glm-5.3.yaml | \
  kubectl apply --namespace "$SNAPSHOT_NAMESPACE" --filename -

kubectl rollout status --namespace "$SNAPSHOT_NAMESPACE" \
  deployment/vllm-glm-5-3-source --timeout=60m
```

Then [create a PodSnapshot](checkpoint.md#option-1--podsnapshot-checkpoint-a-running-replica)
for the ready source Pod. Delete the source Deployment after the checkpoint is
ready to free its GPUs.

## 3. Restore

In `restore/8-gpu.yaml`, set `nvidia.com/restore-from` to the PodSnapshot name,
such as `vllm-glm-5-3-snapshot`, then apply it:

```bash
kubectl apply --namespace "$SNAPSHOT_NAMESPACE" \
  --filename docs/guides/vllm/restore/8-gpu.yaml

kubectl rollout status --namespace "$SNAPSHOT_NAMESPACE" \
  deployment/vllm-8gpu-restored --timeout=30m
```

See the [restore guide](restore.md) to verify the restored replica.
