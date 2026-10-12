// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

package compat

import (
	"sort"
	"strings"
)

// instructionSetFeatures are the CPU flags, as /proc/cpuinfo spells them, that
// say a process may execute some instruction. They are CRIU's own
// instruction-level mask, x86_ins_capability_mask in criu/arch/x86/cpu.c, with
// FMA, which that mask omits, and the extensions newer than it added. Three of
// its entries are left out because they name no instruction a process can run:
// rep_good is a kernel performance hint, tme is memory encryption, and monitor
// is MONITOR/MWAIT, which only ring 0 may execute.
var instructionSetFeatures = featureSet(`
	fpu tsc cx8 sep cmov clflush mmx fxsr sse sse2
	syscall mmxext rdtscp 3dnowext 3dnow nopl
	pni pclmulqdq ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt aes xsave avx f16c rdrand
	abm sse4a misalignsse 3dnowprefetch xop fma4 tbm
	fsgsbase bmi1 hle avx2 bmi2 erms rtm mpx avx512f avx512dq rdseed adx clflushopt
	avx512pf avx512er avx512cd sha_ni avx512bw avx512vl
	xsaveopt xsavec xgetbv1
	avx512vbmi avx512_vbmi2 gfni vaes vpclmulqdq avx512_vnni avx512_bitalg avx512_vpopcntdq rdpid
	clzero avx512_4vnniw avx512_4fmaps
	avx_vnni avx_ifma avx512_bf16 avx512_fp16 avx512_vp2intersect
	amx_bf16 amx_tile amx_int8 amx_fp16 movdiri movdir64b serialize
`)

func featureSet(names string) map[string]bool {
	set := make(map[string]bool)
	for _, name := range strings.Fields(names) {
		set[name] = true
	}
	return set
}

// InstructionSetFeatures keeps the instruction-set flags out of a CPU's flags,
// sorted and without duplicates. The rest describe the machine rather than what
// runs on it - hypervisor, constant_tsc, the vulnerability workarounds - and
// differ between nodes a process moves between freely, so comparing them would
// refuse restores that work. A CPU with none of them is unknown, so the result
// is then nil.
//
// Exported so that what a checkpoint records is the same reduction this
// compares, rather than a second one that can disagree with it.
func InstructionSetFeatures(flags []string) []string {
	seen := make(map[string]bool, len(flags))
	var features []string
	for _, flag := range flags {
		if instructionSetFeatures[flag] && !seen[flag] {
			seen[flag] = true
			features = append(features, flag)
		}
	}
	sort.Strings(features)
	return features
}
