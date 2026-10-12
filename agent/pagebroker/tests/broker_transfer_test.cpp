// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "broker.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <exception>
#include <fstream>
#include <functional>
#include <future>

#include "tests/temporary_directory.hpp"
#include "transfer/engine/posix/posix_copy_engine.hpp"

namespace snapshot::pagebroker {
namespace fs = std::filesystem;
namespace {
enum class Boundary { PrepareRestore, StageRestore, ValidateCheckpoint, InspectCheckpoint, PublishCheckpoint };

// Real filesystem operations with deterministic interruption at engine boundaries.
class InterruptingEngine : public TransferEngine {
 public:
  using Callback = std::function<void(Boundary, TransferControl)>;
  InterruptingEngine(const Path& storage, Callback callback) : posix_(storage), callback_(std::move(callback)) {}
  IoEngine type() const override { return IoEngine::POSIX_COPY; }
  PreparedRestore PrepareRestore(const StorageBackend& source, TransferControl control,
      const PublishedArtifact* artifact, bool metadata_only, TransferMode mode) const override
  {
    callback_(Boundary::PrepareRestore, control);
    return posix_.PrepareRestore(source, control, artifact, metadata_only, mode);
  }
  RestoreAccess StageRestore(PreparedRestore preparation, const Path& destination, TransferControl control) const override
  {
    auto access = posix_.StageRestore(std::move(preparation), destination, control);
    callback_(Boundary::StageRestore, control);
    return access;
  }
  Path PrepareCheckpoint(const StorageBackend& destination, const Path& staging_directory,
      const std::string& transaction_id, TransferMode mode, const PublishedArtifact* artifact,
      TransferControl control) const override
  {
    ValidateCheckpointDestination(destination, artifact, control);
    return posix_.PrepareCheckpoint(destination, staging_directory, transaction_id, mode, artifact, control);
  }
  void ValidateCheckpointDestination(const StorageBackend& destination,
      const PublishedArtifact* artifact, TransferControl control) const override
  {
    callback_(Boundary::ValidateCheckpoint, control);
    posix_.ValidateCheckpointDestination(destination, artifact, control);
  }
  RestorePlan InspectCheckpoint(const Path& source, const PublishedArtifact* artifact,
      TransferControl control) const override
  {
    callback_(Boundary::InspectCheckpoint, control);
    return posix_.InspectCheckpoint(source, artifact, control);
  }
  void PublishCheckpoint(const Path& source, const StorageBackend& destination, RestorePlan plan,
      CheckpointPublication* publication, TransferControl control, TransferMode mode) const override
  {
    callback_(Boundary::PublishCheckpoint, control);
    posix_.PublishCheckpoint(source, destination, std::move(plan), publication, control, mode);
  }

 private:
  PosixCopyEngine posix_;
  Callback callback_;
};

Request
RequestFor(const char* id)
{
  Request request;
  request.set_request_id(std::string("request-") + id);
  request.set_transaction_id(id);
  return request;
}
}  // namespace

class BrokerTransferTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    fs::create_directories(source_);
    std::ofstream(source_ / "image") << "image";
  }

  void Intercept(Broker& broker, InterruptingEngine::Callback callback)
  {
    broker.io_engines_.clear();
    broker.io_engines_.push_back(std::make_unique<InterruptingEngine>(storage_, std::move(callback)));
  }

  Request Restore()
  {
    auto request = RequestFor("restore");
    request.mutable_staged_restore()->mutable_source()->mutable_filesystem()->set_directory(source_.string());
    request.mutable_staged_restore()->mutable_io_engine()->mutable_posix_copy();
    return request;
  }

  std::shared_ptr<TransactionResources> Limit(Broker& broker, std::chrono::steady_clock::duration lifetime)
  {
    broker.resources_ = std::make_shared<TransactionResources>(4, 1, lifetime);
    return broker.resources_;
  }

  std::shared_ptr<Transaction> Find(Broker& broker, const std::string& id)
  {
    return broker.FindTransaction(id);
  }

  std::shared_ptr<Transaction> Seed(Broker& broker, Transaction::Kind kind, Transaction::State state)
  {
    auto transaction = broker.CreateOrGetTransaction("seed");
    std::lock_guard lock(transaction->mutex());
    transaction->PrepareTransfer(broker.resources_, kind, staging_ / "restore/seed");
    transaction->Reserve(4);
    fs::create_directory(staging_ / "restore/seed");
    std::ofstream(staging_ / "restore/seed/image") << "data";
    transaction->set_descriptor(RestoreTransactionDescriptor(staging_ / "restore/seed"));
    transaction->set_state(state);
    return transaction;
  }

  void TryRetention(Broker& broker) { broker.RetainTerminalTransaction("seed"); }

  size_t Retained(Broker& broker)
  {
    std::lock_guard lock(broker.terminal_transactions_mutex_);
    return broker.terminal_transactions_.size();
  }

  void BlockCleanup()
  {
    fs::rename(staging_ / "restore", staging_ / "held");
    std::ofstream(staging_ / "restore") << "blocks traversal";
  }

  void UnblockCleanup()
  {
    fs::remove(staging_ / "restore");
    fs::rename(staging_ / "held", staging_ / "restore");
  }

  test::TemporaryDirectory root_;
  const Path storage_ = root_.path() / "storage";
  const Path source_ = storage_ / "source";
  const Path staging_ = root_.path() / "staging";
};

TEST_F(BrokerTransferTest, DirectRestoreUsesGenericPreparationAndBorrowsItsSource)
{
  Broker broker(staging_, storage_);
  auto resources = Limit(broker, std::chrono::hours(1));
  ASSERT_EQ(mkfifo((source_ / "unscanned").c_str(), 0600), 0);
  std::vector<Boundary> calls;
  Intercept(broker, [&](Boundary boundary, TransferControl) { calls.push_back(boundary); });
  auto request = RequestFor("direct");
  request.mutable_direct_restore()->mutable_source()->mutable_filesystem()->set_directory(source_.string());
  request.mutable_direct_restore()->mutable_io_engine()->mutable_posix_copy();
  ASSERT_TRUE(broker.HandleRequest(request).has_direct_restore_ready());
  EXPECT_EQ(calls, (std::vector<Boundary>{Boundary::PrepareRestore, Boundary::StageRestore}));
  EXPECT_EQ(resources->reserved, 0u);
  const auto transaction = Find(broker, "direct");
  const auto& access = std::get<RestoreTransactionDescriptor>(transaction->descriptor());
  EXPECT_TRUE(access.staging_directory().empty());
  struct stat original{}, retained{};
  ASSERT_EQ(stat(source_.c_str(), &original), 0);
  ASSERT_EQ(fstat(access.source_fd(), &retained), 0);
  EXPECT_EQ(original.st_ino, retained.st_ino);
  // Matching names are not ownership: direct restore never created these trees.
  fs::create_directory(staging_ / "restore/direct");
  fs::create_directory(staging_ / "checkpoint/direct");
  request.mutable_abort();
  ASSERT_TRUE(broker.HandleRequest(request).has_abort_complete());
  EXPECT_TRUE(fs::exists(source_ / "image"));
  EXPECT_TRUE(fs::exists(staging_ / "restore/direct"));
  EXPECT_TRUE(fs::exists(staging_ / "checkpoint/direct"));
}

TEST_F(BrokerTransferTest, BothCheckpointModesUseGenericPreparationAndPublication)
{
  for (const bool direct : {false, true}) {
    SCOPED_TRACE(direct);
    Broker broker(staging_, storage_);
    std::vector<Boundary> calls;
    Intercept(broker, [&](Boundary boundary, TransferControl) { calls.push_back(boundary); });
    auto request = RequestFor(direct ? "direct-save" : "staged-save");
    auto* destination = direct ? request.mutable_prepare_direct_checkpoint()->mutable_destination()
                               : request.mutable_prepare_staged_checkpoint()->mutable_destination();
    auto* engine = direct ? request.mutable_prepare_direct_checkpoint()->mutable_io_engine()
                          : request.mutable_prepare_staged_checkpoint()->mutable_io_engine();
    destination->mutable_filesystem()->set_directory((storage_ / "saved").string());
    engine->mutable_posix_copy();
    const auto prepared = broker.HandleRequest(request);
    ASSERT_FALSE(prepared.has_failure()) << prepared.DebugString();
    const auto output = direct ? prepared.direct_checkpoint_directory().image_directory()
                               : prepared.staged_checkpoint_directory().image_directory();
    std::ofstream(Path(output) / "payload") << "checkpoint";
    request.mutable_commit();
    ASSERT_TRUE(broker.HandleRequest(request).has_commit_complete());
    EXPECT_EQ(calls, (std::vector<Boundary>{Boundary::ValidateCheckpoint, Boundary::InspectCheckpoint,
                                         Boundary::PublishCheckpoint}));
    EXPECT_TRUE(fs::exists(storage_ / "saved/payload"));
    EXPECT_FALSE(fs::exists(output));
  }
}

TEST_F(BrokerTransferTest, FailedDirectRestoreNeverRemovesBorrowedData)
{
  Broker broker(staging_, storage_);
  Intercept(broker, [&](Boundary boundary, TransferControl) {
    if (boundary == Boundary::StageRestore)
      throw std::runtime_error("access failed");
  });
  auto request = RequestFor("direct-failure");
  request.mutable_direct_restore()->mutable_source()->mutable_filesystem()->set_directory(source_.string());
  request.mutable_direct_restore()->mutable_io_engine()->mutable_posix_copy();
  EXPECT_EQ(broker.HandleRequest(request).failure().code(), Failure::STORAGE_ERROR);
  request.mutable_abort();
  EXPECT_TRUE(broker.HandleRequest(request).has_abort_complete());
  EXPECT_TRUE(fs::exists(source_ / "image"));
}

TEST_F(BrokerTransferTest, PreservesFailureCodesAcrossRequestAndTransferBoundaries)
{
  const struct {
    std::exception_ptr error;
    Failure::Code code;
  } failures[] = {
      {std::make_exception_ptr(TransferInterrupted(TransferInterrupted::Reason::CANCELLED)), Failure::INTERNAL_ERROR},
      {std::make_exception_ptr(TransferInterrupted(TransferInterrupted::Reason::DEADLINE_EXCEEDED)), Failure::TRANSACTION_EXPIRED},
      {std::make_exception_ptr(TransferError(Failure::OUTCOME_UNKNOWN, "publication uncertain")), Failure::OUTCOME_UNKNOWN},
      {std::make_exception_ptr(std::runtime_error("storage failed")), Failure::STORAGE_ERROR},
  };
  for (const auto& failure : failures) {
    SCOPED_TRACE(Failure::Code_Name(failure.code));
    for (const auto boundary : {Boundary::PrepareRestore, Boundary::StageRestore, Boundary::ValidateCheckpoint,
                               Boundary::InspectCheckpoint, Boundary::PublishCheckpoint}) {
      SCOPED_TRACE(static_cast<int>(boundary));
      Broker broker(staging_, storage_);
      Intercept(broker, [&](Boundary current, TransferControl) {
        if (current == boundary)
          std::rethrow_exception(failure.error);
      });
      if (boundary == Boundary::PrepareRestore || boundary == Boundary::StageRestore) {
        const auto response = broker.HandleRequest(Restore());
        ASSERT_TRUE(response.has_failure()) << response.DebugString();
        EXPECT_EQ(response.failure().code(), failure.code);
        EXPECT_FALSE(fs::exists(staging_ / "restore" / "restore"));
      } else {
        auto prepare = RequestFor("checkpoint");
        prepare.mutable_prepare_staged_checkpoint()->mutable_destination()->mutable_filesystem()->set_directory(
            (storage_ / "published").string());
        prepare.mutable_prepare_staged_checkpoint()->mutable_io_engine()->mutable_posix_copy();
        auto response = broker.HandleRequest(prepare);
        if (boundary != Boundary::ValidateCheckpoint) {
          ASSERT_TRUE(response.has_staged_checkpoint_directory()) << response.DebugString();
          const Path directory(response.staged_checkpoint_directory().image_directory());
          std::ofstream(directory / "image") << "image";
          auto commit = RequestFor("checkpoint");
          commit.mutable_commit();
          response = broker.HandleRequest(commit);
          EXPECT_TRUE(fs::exists(directory / "image"));
          auto abort = RequestFor("checkpoint");
          abort.mutable_abort();
          EXPECT_TRUE(broker.HandleRequest(abort).has_abort_complete());
          EXPECT_FALSE(fs::exists(directory));
        }
        ASSERT_TRUE(response.has_failure()) << response.DebugString();
        EXPECT_EQ(response.failure().code(), failure.code);
        EXPECT_FALSE(fs::exists(storage_ / "published"));
      }
    }
  }
}

TEST_F(BrokerTransferTest, ShutdownCancelsActiveRestoreAndRemovesStaging)
{
  Broker broker(staging_, storage_);
  std::promise<void> staged, resume;
  Intercept(broker, [&](Boundary boundary, TransferControl control) {
    if (boundary == Boundary::StageRestore) {
      staged.set_value();
      resume.get_future().wait();
      control.Check();
    }
  });
  const auto request = Restore();
  auto restore = std::async(std::launch::async, [&] { return broker.HandleRequest(request); });
  staged.get_future().wait();
  broker.CancelActiveTransactions();
  resume.set_value();
  const auto response = restore.get();
  ASSERT_TRUE(response.has_failure()) << response.DebugString();
  EXPECT_EQ(response.failure().code(), Failure::INTERNAL_ERROR);
  EXPECT_EQ(response.failure().message(), "transfer cancelled");
  EXPECT_FALSE(fs::exists(staging_ / "restore" / "restore"));
}

TEST_F(BrokerTransferTest, PeriodicSweepRetainsMissedCompletionsAndEvictsThemWithoutRequests)
{
  for (const auto state : {Transaction::State::COMMITTED, Transaction::State::ABORTED}) {
    SCOPED_TRACE(static_cast<int>(state));
    Broker broker(staging_, storage_);
    auto transaction = Seed(broker, Transaction::Kind::RESTORE, state);
    {
      std::lock_guard lock(transaction->mutex());
      // Another request owns the mutex when completion tries to retain the result.
      std::async(std::launch::async, [&] { TryRetention(broker); }).get();
      std::async(std::launch::async, [&] { broker.ReapExpiredTransactions(std::chrono::steady_clock::now()); }).get();
      EXPECT_EQ(Retained(broker), 0u);
    }
    broker.ReapExpiredTransactions(std::chrono::steady_clock::now());
    EXPECT_EQ(Retained(broker), 1u);
    broker.ReapExpiredTransactions(std::chrono::steady_clock::now());
    EXPECT_EQ(Retained(broker), 1u);
    broker.ReapExpiredTransactions(std::chrono::steady_clock::now() + std::chrono::hours(2));
    EXPECT_EQ(Retained(broker), 0u);
    EXPECT_EQ(Find(broker, "seed"), nullptr);
  }
}

TEST_F(BrokerTransferTest, ExpiryReclaimsEveryTransferKindAndRestoresAdmission)
{
  for (const auto kind : {Transaction::Kind::CHECKPOINT, Transaction::Kind::RESTORE, Transaction::Kind::METADATA}) {
    SCOPED_TRACE(static_cast<int>(kind));
    Broker broker(staging_, storage_);
    const auto resources = Limit(broker, std::chrono::minutes(1));
    auto transaction = Seed(broker, kind, Transaction::State::STAGED);
    const auto sweep = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    broker.ReapExpiredTransactions(sweep);
    EXPECT_EQ(transaction->state(), Transaction::State::ABORTED);
    EXPECT_TRUE(transaction->control().cancellation.stop_requested());
    EXPECT_FALSE(fs::exists(staging_ / "restore/seed"));
    EXPECT_EQ(resources->active, 0u);
    EXPECT_EQ(resources->reserved, 0u);
    EXPECT_EQ(Retained(broker), 0u);
    EXPECT_EQ(Find(broker, "seed"), nullptr);
    // Another sweep cannot double-release capacity or retain a reaped transaction.
    broker.ReapExpiredTransactions(sweep);
    EXPECT_EQ(resources->active, 0u);
    EXPECT_EQ(Retained(broker), 0u);
    Transaction next;
    ASSERT_NO_THROW(next.PrepareTransfer(resources, kind, staging_ / "next"));
    EXPECT_NO_THROW(next.Reserve(4));
    auto abort = RequestFor("seed");
    abort.mutable_abort();
    EXPECT_EQ(broker.HandleRequest(abort).failure().code(), Failure::TRANSACTION_NOT_FOUND);
    EXPECT_EQ(broker.HandleRequest(abort).failure().code(), Failure::TRANSACTION_NOT_FOUND);
  }
}

TEST_F(BrokerTransferTest, PeriodicCleanupRetriesKeepCapacityUntilRemovalSucceeds)
{
  for (const auto state : {Transaction::State::STAGED, Transaction::State::COMMITTED, Transaction::State::ABORTED}) {
    SCOPED_TRACE(static_cast<int>(state));
    Broker broker(staging_, storage_);
    const auto resources = Limit(broker, std::chrono::minutes(1));
    auto transaction = Seed(broker, Transaction::Kind::RESTORE, state);
    BlockCleanup();
    const auto sweep = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    for (unsigned retry = 0; retry < 2; ++retry) {
      broker.ReapExpiredTransactions(sweep);
      EXPECT_EQ(transaction->state(), state == Transaction::State::STAGED ? Transaction::State::ABORTING : state);
      EXPECT_TRUE(std::holds_alternative<RestoreTransactionDescriptor>(transaction->descriptor()));
      EXPECT_EQ(resources->active, 1u);
      EXPECT_EQ(resources->reserved, 4u);
      EXPECT_EQ(Retained(broker), 0u);
      Transaction next;
      EXPECT_THROW(next.PrepareTransfer(resources, Transaction::Kind::RESTORE, staging_ / "next"), TransferError);
    }
    UnblockCleanup();
    broker.ReapExpiredTransactions(sweep);
    EXPECT_FALSE(fs::exists(staging_ / "restore/seed"));
    EXPECT_EQ(resources->active, 0u);
    EXPECT_EQ(resources->reserved, 0u);
    EXPECT_EQ(Retained(broker), state == Transaction::State::STAGED ? 0u : 1u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(transaction->descriptor()));
  }
}

TEST_F(BrokerTransferTest, ExpiryWaitsForGpuDrainBeforeRemovingStaging)
{
  Broker broker(staging_, storage_);
  const auto resources = Limit(broker, std::chrono::minutes(1));
  auto transaction = Seed(broker, Transaction::Kind::RESTORE, Transaction::State::STAGED);
  auto operation = std::make_shared<Transaction::GpuOperation>();
  operation->state = Transaction::GpuOperation::State::Running;
  operation->cancellation = std::make_shared<Cancellation>();
  transaction->gpu_operation = operation;
  const auto sweep = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  broker.ReapExpiredTransactions(sweep);
  EXPECT_TRUE(operation->cancellation->IsCancelled());
  EXPECT_EQ(transaction->state(), Transaction::State::ABORTING);
  EXPECT_TRUE(fs::exists(staging_ / "restore/seed/image"));
  EXPECT_EQ(resources->active, 1u);
  EXPECT_EQ(resources->reserved, 4u);
  broker.SetClock([sweep] { return sweep; });
  auto request = RequestFor("seed");
  request.mutable_commit();
  EXPECT_EQ(broker.HandleRequest(request).failure().code(), Failure::TRANSACTION_EXPIRED);
  request.mutable_abort();
  EXPECT_EQ(broker.HandleRequest(request).failure().code(), Failure::TRANSACTION_EXPIRED);
  EXPECT_TRUE(fs::exists(staging_ / "restore/seed/image"));
  operation->state = Transaction::GpuOperation::State::Finished;
  broker.ReapExpiredTransactions(sweep);
  EXPECT_FALSE(fs::exists(staging_ / "restore/seed"));
  EXPECT_EQ(resources->active, 0u);
  EXPECT_EQ(resources->reserved, 0u);
  EXPECT_EQ(broker.HandleRequest(request).failure().code(), Failure::TRANSACTION_NOT_FOUND);
}

TEST_F(BrokerTransferTest, ExpirySkipsActiveTransfersUntilTheyDrain)
{
  std::ofstream(source_ / "image") << "data";
  Broker broker(staging_, storage_);
  const auto resources = Limit(broker, std::chrono::minutes(1));
  std::promise<void> staged, resume;
  Intercept(broker, [&](Boundary boundary, TransferControl) {
    if (boundary == Boundary::StageRestore) {
      staged.set_value();
      resume.get_future().wait();
    }
  });
  const auto request = Restore();
  auto restore = std::async(std::launch::async, [&] { return broker.HandleRequest(request); });
  staged.get_future().wait();
  const auto sweep = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  broker.ReapExpiredTransactions(sweep);
  EXPECT_TRUE(fs::exists(staging_ / "restore/restore/image"));
  EXPECT_EQ(resources->active, 1u);
  EXPECT_EQ(resources->reserved, 4u);
  resume.set_value();
  ASSERT_TRUE(restore.get().has_staged_restore_directory());
  broker.ReapExpiredTransactions(sweep);
  EXPECT_FALSE(fs::exists(staging_ / "restore/restore"));
  EXPECT_EQ(resources->active, 0u);
  EXPECT_EQ(resources->reserved, 0u);
}

TEST_F(BrokerTransferTest, AbortAfterDeadlineLeavesCleanupToTheReaper)
{
  Broker broker(staging_, storage_);
  const auto resources = Limit(broker, std::chrono::seconds(0));
  auto transaction = Seed(broker, Transaction::Kind::RESTORE, Transaction::State::STAGED);
  auto abort = RequestFor("seed");
  abort.mutable_abort();
  BlockCleanup();
  EXPECT_EQ(broker.HandleRequest(abort).failure().code(), Failure::TRANSACTION_EXPIRED);
  EXPECT_EQ(transaction->state(), Transaction::State::STAGED);
  broker.ReapExpiredTransactions(std::chrono::steady_clock::now());
  EXPECT_EQ(transaction->state(), Transaction::State::ABORTING);
  EXPECT_EQ(resources->active, 1u);
  UnblockCleanup();
  EXPECT_EQ(broker.HandleRequest(abort).failure().code(), Failure::TRANSACTION_EXPIRED);
  EXPECT_TRUE(fs::exists(staging_ / "restore/seed/image"));
  broker.ReapExpiredTransactions(std::chrono::steady_clock::now());
  EXPECT_EQ(broker.HandleRequest(abort).failure().code(), Failure::TRANSACTION_NOT_FOUND);
  EXPECT_EQ(resources->active, 0u);
  EXPECT_EQ(resources->reserved, 0u);
  auto commit = RequestFor("seed");
  commit.mutable_commit();
  EXPECT_EQ(broker.HandleRequest(commit).failure().code(), Failure::TRANSACTION_NOT_FOUND);
}

TEST_F(BrokerTransferTest, ExpiredCommitLeavesCleanupAndItsRetriesToTheReaper)
{
  Broker broker(staging_, storage_);
  const auto resources = Limit(broker, std::chrono::seconds(0));
  auto transaction = Seed(broker, Transaction::Kind::RESTORE, Transaction::State::STAGED);
  auto commit = RequestFor("seed");
  commit.mutable_commit();
  BlockCleanup();
  EXPECT_EQ(broker.HandleRequest(commit).failure().code(), Failure::TRANSACTION_EXPIRED);
  EXPECT_EQ(transaction->state(), Transaction::State::STAGED);
  broker.ReapExpiredTransactions(std::chrono::steady_clock::now());
  EXPECT_EQ(transaction->state(), Transaction::State::ABORTING);
  EXPECT_EQ(resources->active, 1u);
  EXPECT_EQ(Retained(broker), 0u);
  UnblockCleanup();
  broker.ReapExpiredTransactions(std::chrono::steady_clock::now());
  EXPECT_EQ(transaction->state(), Transaction::State::ABORTED);
  EXPECT_EQ(resources->active, 0u);
  EXPECT_FALSE(fs::exists(staging_ / "restore/seed"));
  EXPECT_EQ(broker.HandleRequest(commit).failure().code(), Failure::TRANSACTION_NOT_FOUND);
}
}  // namespace snapshot::pagebroker
