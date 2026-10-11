// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Package nri makes snapshot-restore-proxy PID 1 in restore destination
// containers. It is an NRI plugin: on container creation it copies the proxy
// into the container's control directory and replaces the container's command
// with it, before the container starts. The Pod object is not changed.
package nri

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"slices"
	"sync"
	"time"

	"github.com/containerd/nri/pkg/api"
	"github.com/containerd/nri/pkg/stub"
	"github.com/go-logr/logr"
	corev1 "k8s.io/api/core/v1"
	ktypes "k8s.io/apimachinery/pkg/types"

	"github.com/ai-dynamo/snapshot/agent/internal/nsmount"
	snapshotruntime "github.com/ai-dynamo/snapshot/agent/internal/runtime"
	"github.com/ai-dynamo/snapshot/api/podcontract"
)

const (
	// PluginName and PluginIndex register the plugin with the runtime. The
	// index orders it among other NRI plugins on the node.
	PluginName  = "snapshot"
	PluginIndex = "50"

	// ProxyBinaryPath is where the agent image ships the proxy.
	ProxyBinaryPath = nsmount.SnapshotBinSrc + "/" + podcontract.RestoreProxyBinaryName

	// lookupTimeout bounds the API reads for one container. They run inside
	// the runtime's CreateContainer call, which NRI also bounds.
	lookupTimeout = time.Second
)

// vCluster syncs a Pod to the host under another name, namespace and UID, and
// NRI reports the host Pod. vCluster records the Pod's own identity in these
// annotations on the host Pod.
const (
	vclusterNamespaceAnnotation = "vcluster.loft.sh/object-namespace"
	vclusterNameAnnotation      = "vcluster.loft.sh/object-name"
	vclusterUIDAnnotation       = "vcluster.loft.sh/object-uid"
)

// proxyCommand is the proxy as the container sees it: the control directory
// is mounted at SnapshotControlMountPath.
var proxyCommand = []string{path.Join(podcontract.SnapshotControlMountPath, podcontract.RestoreProxyBinaryName)}

// DestinationResolver finds restore Pods in the agent's own cluster, and the
// containers they restore into.
type DestinationResolver interface {
	// RestorePod returns nil if the agent's cluster has no such Pod.
	RestorePod(ctx context.Context, namespace, name string, uid ktypes.UID) (*corev1.Pod, error)
	RestoreDestinations(ctx context.Context, pod *corev1.Pod) ([]string, error)
}

// Plugin is the NRI plugin.
type Plugin struct {
	resolver DestinationResolver
	log      logr.Logger
	// openControlDir and proxyPath are replaced in tests.
	openControlDir func(podUID, containerName string) (*os.File, error)
	proxyPath      string
}

// NewPlugin returns a plugin that resolves destinations through resolver.
func NewPlugin(resolver DestinationResolver, log logr.Logger) *Plugin {
	return &Plugin{
		resolver:       resolver,
		log:            log,
		openControlDir: snapshotruntime.OpenPodControlDir,
		proxyPath:      ProxyBinaryPath,
	}
}

// Run registers with the runtime and serves until ctx ends. It returns an
// error if registration fails or the connection to the runtime drops: a node
// that cannot shape restore containers must not look healthy.
func (p *Plugin) Run(ctx context.Context) error {
	closed := make(chan struct{})
	var once sync.Once
	s, err := stub.New(p,
		stub.WithPluginName(PluginName),
		stub.WithPluginIdx(PluginIndex),
		stub.WithOnClose(func() { once.Do(func() { close(closed) }) }),
	)
	if err != nil {
		return fmt.Errorf("create NRI plugin: %w", err)
	}
	ran := make(chan error, 1)
	go func() { ran <- s.Run(ctx) }()
	select {
	case <-closed:
		s.Stop()
		return errors.New("NRI connection to the container runtime closed")
	case err := <-ran:
		if ctx.Err() != nil {
			return nil
		}
		if err == nil {
			err = errors.New("stopped")
		}
		return fmt.Errorf("NRI plugin: %w", err)
	}
}

// CreateContainer shapes restore destination containers and leaves every
// other container alone. An error fails the container's creation, and kubelet
// retries it: running the image's own command instead would start the
// workload from scratch in a container the agent is about to restore into.
func (p *Plugin) CreateContainer(ctx context.Context, pod *api.PodSandbox, ctr *api.Container) (*api.ContainerAdjustment, []*api.ContainerUpdate, error) {
	if _, ok := pod.GetAnnotations()[podcontract.RestoreFromAnnotation]; !ok {
		return nil, nil, nil
	}
	namespace, name, uid := podIdentity(pod)
	log := p.log.WithValues("pod", namespace+"/"+name, "container", ctr.GetName())

	lookupCtx, cancel := context.WithTimeout(ctx, lookupTimeout)
	defer cancel()
	// The runtime may also run Pods of other clusters that share the node.
	// Those are left alone: their own Snapshot install, if any, handles them.
	restorePod, err := p.resolver.RestorePod(lookupCtx, namespace, name, ktypes.UID(uid))
	if err != nil {
		log.Error(err, "Cannot look up restore Pod")
		return nil, nil, fmt.Errorf("snapshot: look up restore Pod: %w", err)
	}
	if restorePod == nil {
		log.V(1).Info("Restore Pod is not in this cluster; leaving it alone")
		return nil, nil, nil
	}
	destinations, err := p.resolver.RestoreDestinations(lookupCtx, restorePod)
	if err != nil {
		log.Error(err, "Cannot resolve restore destinations")
		return nil, nil, fmt.Errorf("snapshot: resolve restore destinations: %w", err)
	}
	if !slices.Contains(destinations, ctr.GetName()) {
		return nil, nil, nil
	}

	// kubelet's directories use the UID of the Pod on the node.
	dir, err := p.openControlDir(pod.GetUid(), ctr.GetName())
	if err == nil {
		err = installProxy(p.proxyPath, dir)
		_ = dir.Close()
	}
	if err != nil {
		log.Error(err, "Cannot install restore proxy")
		return nil, nil, fmt.Errorf("snapshot: install restore proxy: %w", err)
	}

	adjust := &api.ContainerAdjustment{}
	adjust.SetArgs(proxyCommand)
	log.Info("Restore proxy is PID 1", "control_dir", dir.Name())
	return adjust, nil, nil
}

// podIdentity returns the Pod's namespace, name and UID in its own cluster.
func podIdentity(pod *api.PodSandbox) (namespace, name, uid string) {
	a := pod.GetAnnotations()
	namespace, name, uid = a[vclusterNamespaceAnnotation], a[vclusterNameAnnotation], a[vclusterUIDAnnotation]
	if namespace == "" || name == "" || uid == "" {
		return pod.GetNamespace(), pod.GetName(), pod.GetUid()
	}
	return namespace, name, uid
}

// installProxy copies the proxy binary into dir. It copies on every creation:
// the emptyDir is per Pod, and the binary is a few MB.
func installProxy(src string, dir *os.File) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	return snapshotruntime.WriteFileInDir(dir, podcontract.RestoreProxyBinaryName, 0o755, func(w io.Writer) error {
		_, err := io.Copy(w, in)
		return err
	})
}
