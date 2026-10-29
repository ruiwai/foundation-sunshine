#include "src/capture_control.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

TEST(CaptureControlTest, IndependentObserversSeeLatestRequest) {
  capture_control::display_switch_t requests;
  auto a = requests.generation();
  auto b = a;
  requests.request(1);
  requests.request(3);
  EXPECT_TRUE(requests.pending(a));
  EXPECT_EQ(requests.poll(a), 3);
  EXPECT_FALSE(requests.pending(a));
  EXPECT_TRUE(requests.pending(b));
  EXPECT_EQ(requests.poll(b), 3);
  EXPECT_EQ(a, b);
}

TEST(CaptureControlTest, RepeatedIndexAdvancesGeneration) {
  capture_control::display_switch_t requests;
  auto seen = requests.generation();
  requests.request(2);
  EXPECT_EQ(requests.poll(seen), 2);
  auto previous = seen;
  requests.request(2);
  EXPECT_EQ(requests.generation(), previous + 1);
  EXPECT_EQ(requests.poll(seen), 2);
}

TEST(CaptureControlTest, NewOwnerIgnoresHistory) {
  capture_control::display_switch_t requests;
  requests.request(7);
  auto seen = requests.generation();
  EXPECT_FALSE(requests.pending(seen));
  EXPECT_FALSE(requests.poll(seen));
  requests.request(4);
  EXPECT_EQ(requests.poll(seen), 4);
}

TEST(CaptureControlTest, AbsentPollReturnsWithoutEvent) {
  capture_control::display_switch_t requests;
  auto seen = requests.generation();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) EXPECT_FALSE(requests.poll(seen));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
  EXPECT_EQ(seen, 0u);
}

TEST(CaptureControlTest, SingletonSupportsSynchronizedPublisher) {
  auto &requests = capture_control::display_switch;
  auto a = requests.generation();
  auto b = a;
  std::thread producer([&]() {
    for (int i = 0; i < 1000; ++i) requests.request(i);
  });
  while (requests.generation() == a) std::this_thread::yield();
  EXPECT_TRUE(requests.pending(b));
  producer.join();
  EXPECT_EQ(requests.poll(a), 999);
  EXPECT_EQ(requests.poll(b), 999);
}
