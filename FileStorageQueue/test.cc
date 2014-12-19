#include <atomic>

#include "fsq.h"

#include "../Bricks/file/file.h"

#include "../Bricks/3party/gtest/gtest.h"
#include "../Bricks/3party/gtest/gtest-main.h"

using std::string;
using std::atomic_bool;

const char* const kTestDir = "build/";

struct MockProcessor {
  MockProcessor() : updated(false) {
  }
  template <typename T_TIMESTAMP, typename T_TIME_SPAN>
  fsq::FileProcessingResult OnFileReady(const std::string& file_name,
                                        const std::string& file_base_name,
                                        uint64_t /*size*/,
                                        T_TIMESTAMP /*created*/,
                                        T_TIME_SPAN /*age*/,
                                        T_TIMESTAMP /*now*/) {
    filename = file_base_name;
    contents = bricks::ReadFileAsString(file_name);
    updated = true;
    return fsq::FileProcessingResult::Success;
  }

  atomic_bool updated;
  string filename = "";
  string contents = "";
};

struct MockTime {
  typedef uint64_t T_TIMESTAMP;
  typedef int64_t T_TIME_SPAN;
  uint64_t now = 0;
  T_TIMESTAMP Now() const {
    return now;
  }
};

struct MockConfig : fsq::Config<MockProcessor> {
  // Mock time.
  typedef MockTime T_TIME_MANAGER;
  // Append using newlines.
  typedef fsq::strategy::AppendToFileWithSeparator T_FILE_APPEND_POLICY;
  // No backlog: 20 bytes 10 seconds old files max, with backlog: 100 bytes 60 seconds old files max.
  typedef fsq::strategy::SimpleFinalizationPolicy<MockTime::T_TIMESTAMP,
                                                  MockTime::T_TIME_SPAN,
                                                  20,
                                                  MockTime::T_TIME_SPAN(10 * 1000),
                                                  100,
                                                  MockTime::T_TIME_SPAN(60 * 1000)> T_FINALIZE_POLICY;
  // Purge after 1000 bytes total or after 3 files.
  typedef fsq::strategy::SimplePurgePolicy<1000, 3> T_PURGE_POLICY;
};

typedef fsq::FSQ<MockConfig> FSQ;

TEST(FileSystemQueueTest, SimpleSmokeTest) {
  // A simple way to create and initialize FileSystemQueue ("FSQ").
  MockProcessor processor;
  MockTime time_manager;
  bricks::FileSystem file_system;
  FSQ fsq(processor, kTestDir, time_manager, file_system);
  fsq.SetSeparator("\n");

  // Confirm the queue is empty.
  EXPECT_EQ(0ull, fsq.GetQueueStatus().appended_file_size);
  EXPECT_EQ(0u, fsq.GetQueueStatus().finalized.queue.size());
  EXPECT_EQ(0ul, fsq.GetQueueStatus().finalized.total_size);

  // Add a few entries.
  time_manager.now = 1001;
  fsq.PushMessage("foo");
  time_manager.now = 1002;
  fsq.PushMessage("bar");
  time_manager.now = 1003;
  fsq.PushMessage("baz");
  time_manager.now = 1010;

  // Confirm the queue is empty.
  EXPECT_EQ(12ull, fsq.GetQueueStatus().appended_file_size);  // Three messages of (3 + '\n') bytes each.
  EXPECT_EQ(0u, fsq.GetQueueStatus().finalized.queue.size());
  EXPECT_EQ(0ul, fsq.GetQueueStatus().finalized.total_size);

  // Force entries processing to have three freshly added ones reach our MockProcessor.
  fsq.ForceResumeProcessing();
  while (!processor.updated) {
    ;  // Spin lock.
  }

  EXPECT_EQ("finalized-00000000000000001001.bin", processor.filename);
  EXPECT_EQ("foo\nbar\nbaz\n", processor.contents);
}

/*

TEST(FileSystemQueueTest, KeepsSameFile);
TEST(FileSystemQueueTest, RenamedFileBecauseOfSize);
TEST(FileSystemQueueTest, RenamedFileBecauseOfAge);
TEST(FileSystemQueueTest, Scan the directory on startup.);
TEST(FileSystemQueueTest, Resume already existing append-only file.);
TEST(FileSystemQueueTest, Correctly extract timestamps from all the files, including the temporary one.);
TEST(FileSystemQueueTest, Rename the current file right away if it should be renamed, before any work.);
TEST(FileSystemQueueTest, Time skew.);
TEST(FileSystemQueueTest, Custom finalize strategy.);
TEST(FileSystemQueueTest, Custom append strategy.);

*/
