// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "file_descriptor.hpp"
#include "transfer/restore_plan.hpp"

namespace snapshot::pagebroker {
enum class TransferMode { Staged, Direct };

// Preparation retains borrowed local access separately from the files to transfer.
struct PreparedRestore {
  RestorePlan plan;
  TransferMode mode = TransferMode::Staged;
  Path local_source;
  FileDescriptor source{-1};
};

// Only staging_directory is owned by the transaction and eligible for cleanup.
struct RestoreAccess {
  Path image_directory;
  Path staging_directory;
  FileDescriptor source{-1};
};
}  // namespace snapshot::pagebroker
