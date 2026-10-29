#include "src/cursor_channel.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace {
  using namespace cursor_channel;
  class CursorChannelTest: public ::testing::Test {
  protected:
    void SetUp() override {
      for (auto id : {101u, 102u, 103u}) remove_session(id);
      set_producer_available(false);
    }
    void TearDown() override { SetUp(); }
    void shape(std::uint32_t id) {
      publish_shape(true, id, 1, 1, 0, 0, {static_cast<std::uint8_t>(id), 0, 0, 255});
    }
  };

  TEST_F(CursorChannelTest, LegacyApiRetainsAvailabilityShapeAndVisibility) {
    EXPECT_FALSE(producer_available());
    set_session_enabled(101, true);
    EXPECT_TRUE(local_mode_active());
    EXPECT_TRUE(producer_local_mode_active());
    set_producer_available(true);
    EXPECT_TRUE(producer_available(101));
    shape(9);
    snapshot_t snapshot;
    ASSERT_TRUE(copy_latest(0, snapshot));
    EXPECT_EQ(snapshot.bgra[0], 9);
    EXPECT_FALSE(copy_latest(snapshot.revision, snapshot));
    publish_visibility(false, 9);
    ASSERT_TRUE(copy_latest(snapshot.revision, snapshot));
    EXPECT_FALSE(snapshot.visible);
    EXPECT_TRUE(snapshot.has_shape);
    publish_visibility(true, 10);
    ASSERT_TRUE(copy_latest(snapshot.revision, snapshot));
    EXPECT_FALSE(snapshot.has_shape);
    set_session_enabled(101, false);
    EXPECT_FALSE(copy_latest(0, snapshot));
    EXPECT_FALSE(local_mode_active());
  }

  TEST_F(CursorChannelTest, PrivateFactoryFailureDoesNotClobberLegacy) {
    set_session_enabled(101, true);
    set_producer_available(true);
    shape(11);
    register_private_session(102);
    EXPECT_THROW({
      producer_scope scope(102);
      EXPECT_FALSE(producer_available(102));
      set_producer_available(false);
      throw std::runtime_error("factory failed");
    }, std::runtime_error);
    EXPECT_TRUE(producer_available());
    EXPECT_TRUE(producer_available(101));
    EXPECT_FALSE(producer_available(102));
    snapshot_t snapshot;
    ASSERT_TRUE(copy_latest(0, snapshot));
    EXPECT_EQ(snapshot.shape_id, 11u);
    EXPECT_FALSE(copy_latest(0, snapshot, 102));
    remove_session(102);
    EXPECT_TRUE(producer_available(102));
  }

  TEST_F(CursorChannelTest, PrivateShapesAndAvailabilityAreIsolated) {
    register_private_session(101);
    register_private_session(102);
    producer_scope first(101);
    set_session_enabled(101, true);
    set_producer_available(true);
    shape(21);
    snapshot_t a, b;
    ASSERT_TRUE(copy_latest(0, a, 101));
    {
      producer_scope second(102);
      set_session_enabled(102, true);
      set_producer_available(true);
      shape(22);
      ASSERT_TRUE(copy_latest(0, b, 102));
      EXPECT_EQ(b.bgra[0], 22);
      EXPECT_GT(b.revision, a.revision);
      EXPECT_FALSE(copy_latest(a.revision, a, 101));
      set_session_enabled(102, false);
      EXPECT_FALSE(copy_latest(0, b, 102));
      ASSERT_TRUE(copy_latest(0, a, 101));
      EXPECT_EQ(a.shape_id, 21u);
      set_session_enabled(102, true);
      shape(23);
    }
    EXPECT_FALSE(producer_available(102));
    EXPECT_TRUE(producer_available(101));
    shape(24); // Restored TLS publisher is the first owner.
    ASSERT_TRUE(copy_latest(a.revision, a, 101));
    EXPECT_EQ(a.shape_id, 24u);
    EXPECT_FALSE(copy_latest(0, b, 102));
  }

  TEST_F(CursorChannelTest, LocalModeIsPerOwnerButActivityIsGlobal) {
    register_private_session(101);
    register_private_session(102);
    set_session_enabled(103, true);
    {
      producer_scope first(101);
      EXPECT_TRUE(local_mode_active());
      EXPECT_FALSE(producer_local_mode_active());
      set_session_enabled(101, true);
      EXPECT_TRUE(producer_local_mode_active());
      {
        producer_scope second(102);
        EXPECT_FALSE(producer_local_mode_active());
        set_session_enabled(102, true);
        EXPECT_TRUE(producer_local_mode_active());
        set_session_enabled(101, false);
        EXPECT_TRUE(producer_local_mode_active());
      }
      EXPECT_FALSE(producer_local_mode_active());
    }
    EXPECT_TRUE(producer_local_mode_active());
  }

  TEST_F(CursorChannelTest, ScopeExitKeepsBindingAndRebindChangesRevision) {
    register_private_session(101);
    set_session_enabled(103, true);
    set_producer_available(true);
    shape(31);
    snapshot_t snapshot;
    std::uint64_t old_revision;
    {
      producer_scope private_owner(101);
      set_session_enabled(101, true);
      shape(32);
      ASSERT_TRUE(copy_latest(0, snapshot, 101));
      old_revision = snapshot.revision;
    }
    EXPECT_FALSE(copy_latest(0, snapshot, 101));
    EXPECT_FALSE(producer_available(101));
    EXPECT_TRUE(producer_available());
    {
      producer_scope rebound(101);
      shape(33);
      ASSERT_TRUE(copy_latest(old_revision, snapshot, 101));
      EXPECT_GT(snapshot.revision, old_revision);
    }
  }

  TEST_F(CursorChannelTest, ThreadScopedPublishersDoNotShareState) {
    register_private_session(101);
    register_private_session(102);
    auto publish = [&](std::uint32_t id) {
      producer_scope scope(id);
      set_session_enabled(id, true);
      set_producer_available(true);
      for (int i = 0; i < 1000; ++i) {
        shape(id);
        snapshot_t snapshot;
        ASSERT_TRUE(copy_latest(0, snapshot, id));
        EXPECT_EQ(snapshot.shape_id, id);
      }
    };
    std::thread a(publish, 101), b(publish, 102);
    a.join();
    b.join();
    EXPECT_FALSE(producer_available(101));
    EXPECT_FALSE(producer_available(102));
  }

  TEST_F(CursorChannelTest, AdmissionBeforeWorkerNeverBorrowsLegacyShapeOrAvailability) {
    set_session_enabled(101, true);
    set_producer_available(true);
    shape(7);
    snapshot_t legacy, private_snapshot;
    ASSERT_TRUE(copy_latest(0, legacy, 101));
    register_private_session(102);
    set_session_enabled(102, true);
    EXPECT_FALSE(producer_available(102));
    EXPECT_FALSE(copy_latest(0, private_snapshot, 102));
    EXPECT_TRUE(producer_available(101));
    EXPECT_FALSE(copy_latest(legacy.revision, legacy, 101));
    {
      producer_scope worker(102);
      EXPECT_FALSE(producer_available(102));
      set_producer_available(true);
      publish_shape(true, 7, 1, 1, 0, 0, {42, 0, 0, 255});
      ASSERT_TRUE(copy_latest(legacy.revision, private_snapshot, 102));
      EXPECT_EQ(private_snapshot.shape_id, legacy.shape_id);
      EXPECT_EQ(private_snapshot.bgra[0], 42);
      EXPECT_GT(private_snapshot.revision, legacy.revision);
      EXPECT_TRUE(producer_available(102));
    }
    EXPECT_FALSE(producer_available(102));
    EXPECT_FALSE(copy_latest(0, private_snapshot, 102));
    EXPECT_FALSE(copy_latest(legacy.revision, legacy, 101));
    EXPECT_TRUE(producer_available(101));
    EXPECT_EQ(legacy.bgra[0], 7);
  }

  TEST_F(CursorChannelTest, RemovedAdmissionCannotBeRevivedAndDuplicateOwnersAreRejected) {
    EXPECT_THROW(register_private_session(0), std::invalid_argument);
    EXPECT_THROW({ producer_scope zero(0); }, std::logic_error);
    EXPECT_THROW({ producer_scope unregistered(102); }, std::logic_error);
    register_private_session(102);
    EXPECT_THROW(register_private_session(102), std::logic_error);
    remove_session(102);
    EXPECT_THROW({ producer_scope late_worker(102); }, std::logic_error);
    register_private_session(102); // Removal and the failed scope left no binding.
    {
      producer_scope worker(102); // Nor an orphan producer.
      set_session_enabled(102, true);
      set_producer_available(true);
      shape(8);
      snapshot_t snapshot;
      ASSERT_TRUE(copy_latest(0, snapshot, 102));
      EXPECT_THROW(register_private_session(102), std::logic_error);
      EXPECT_THROW({ producer_scope duplicate(102); }, std::logic_error);
      EXPECT_TRUE(producer_available(102));
      EXPECT_FALSE(copy_latest(snapshot.revision, snapshot, 102));
      shape(9); // Failed constructors did not change TLS or erase the live owner.
      ASSERT_TRUE(copy_latest(snapshot.revision, snapshot, 102));
      EXPECT_EQ(snapshot.shape_id, 9u);
    }
    EXPECT_FALSE(producer_available(102));
  }
}  // namespace
