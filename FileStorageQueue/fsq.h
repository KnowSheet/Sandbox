// Class FSQ manages local, filesystem-based message queue.
//
// A temporary append-only file is created and then written into. Once the strategy dictates so,
// it is declared finalized and gets atomically renamed to a different name (with its 1st timestamp in it),
// using which name it is passed to the PROCESSOR. A new new append-only file is started in the meantime.
//
// The processor runs in a dedicated thread. Thus, it is guaranteed to process at most one file at a time.
// It can take as long as it needs to process the file. Files are guaranteed to be passed in the FIFO order.
//
// Once a file is ready, which translates to "on startup" if there are pending files,
// the user handler in PROCESSOR::OnFileReady(file_name) is invoked.
// Further logic depends on its return value:
//
// On `Success`, FQS deleted file that just got processed and sends the next one to as it arrives,
// which can be instantaneously, is the queue is not empty, or once the next file is ready, if it is.
//
// On `SuccessAndMoved`, FQS does the same thing as for `Success`, except for it does not attempt
// to delete the file, assuming that it has already been deleted or moved away by the user code.
//
// On `Unavailable`, automatic file processing is suspended until it is resumed externally.
// An example of this case would be the processor being the file uploader, with the device going offline.
// This way, no further action is required until FQS is explicitly notified that the device is back online.
//
// On `FailureNeedRetry`, the file is kept and will be re-attempted to be sent to the processor,
// with respect to the retry strategy specified as the template parameter to FSQ.
//
// On top of the above FSQ keeps an eye on the size it occupies on disk and purges the oldest data files
// if the specified purge strategy dictates so.

#ifndef FSQ_H
#define FSQ_H

#include <cassert>  // TODO(dkorolev): Perhaps introduce exceptions instead of ASSERT-s?

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "status.h"
#include "config.h"
#include "strategies.h"

#include "../Bricks/file/file.h"
#include "../Bricks/time/time.h"

namespace fsq {

enum class FileProcessingResult { Success, SuccessAndMoved, Unavailable, FailureNeerRetry };

template <class CONFIG>
class FSQ final : public CONFIG::T_FILE_NAMING_STRATEGY,
                  public CONFIG::T_FINALIZE_POLICY,
                  public CONFIG::T_PURGE_POLICY,
                  public CONFIG::T_FILE_APPEND_POLICY {
 public:
  typedef CONFIG T_CONFIG;

  typedef typename T_CONFIG::T_PROCESSOR T_PROCESSOR;
  typedef typename T_CONFIG::T_MESSAGE T_MESSAGE;
  typedef typename T_CONFIG::T_FILE_APPEND_POLICY T_FILE_APPEND_POLICY;
  typedef typename T_CONFIG::T_FILE_NAMING_STRATEGY T_FILE_NAMING_STRATEGY;
  typedef typename T_CONFIG::T_FILE_SYSTEM T_FILE_SYSTEM;
  typedef typename T_CONFIG::T_TIME_MANAGER T_TIME_MANAGER;
  typedef typename T_CONFIG::T_FINALIZE_POLICY T_FINALIZE_POLICY;
  typedef typename T_CONFIG::T_PURGE_POLICY T_PURGE_POLICY;
  template <class TIME_MANAGER, class FILE_SYSTEM>
  using T_RETRY_POLICY = typename T_CONFIG::template T_RETRY_POLICY<TIME_MANAGER, FILE_SYSTEM>;

  typedef typename T_TIME_MANAGER::T_TIMESTAMP T_TIMESTAMP;
  typedef typename T_TIME_MANAGER::T_TIME_SPAN T_TIME_SPAN;

  typedef QueueFinalizedFilesStatus<T_TIMESTAMP> FinalizedFilesStatus;
  typedef QueueStatus<T_TIMESTAMP> Status;

  // The constructor initializes all the parameters and starts the worker thread.
  FSQ(T_PROCESSOR& processor,
      const std::string& working_directory,
      const T_TIME_MANAGER& time_manager = T_TIME_MANAGER(),
      const T_FILE_SYSTEM& file_system = T_FILE_SYSTEM())
      : processor_(processor),
        working_directory_(working_directory),
        time_manager_(time_manager),
        file_system_(file_system) {
    T_CONFIG::Initialize(*this);
    worker_thread_ = std::thread(&FSQ::WorkerThread, this);
  }

  // Destructor gracefully terminates worker thread and optionally joins it.
  ~FSQ() {
    // Notify the worker thread that it's time to wrap up.
    {
      std::unique_lock<std::mutex> lock(status_mutex_);
      force_worker_thread_shutdown_ = true;
      queue_status_condition_variable_.notify_all();
    }
    // Close the current file. `current_file_.reset(nullptr);` is always safe especially in destructor.
    current_file_.reset(nullptr);
    // Either wait for the processor thread to terminate or detach it.
    if (T_CONFIG::DetachProcessingThreadOnTermination()) {
      worker_thread_.detach();
    } else {
      worker_thread_.join();
    }
  }

  // Getters.
  const std::string& WorkingDirectory() const {
    return working_directory_;
  }

  const Status GetQueueStatus() const {
    // TODO(dkorolev): Wait until the 1st scan, running in a different thread, has finished.
    std::unique_lock<std::mutex> lock(status_mutex_);
    while (!status_ready_) {
      queue_status_condition_variable_.wait(lock);
      // TODO(dkorolev): Handle `force_worker_thread_shutdown_` here, throw an exception.
    }
    // Returning `status_` by const reference is not thread-safe, return a copy from a locked section.
    return status_;
  }

  // `PushMessage()` appends data to the queue.
  void PushMessage(const T_MESSAGE& message) {
    if (force_worker_thread_shutdown_) {
      // TODO(dkorolev): Throw an exception.
      return;
    } else {
      const T_TIMESTAMP now = time_manager_.Now();
      const uint64_t message_size_in_bytes = T_FILE_APPEND_POLICY::MessageSizeInBytes(message);
      EnsureCurrentFileIsOpen(message_size_in_bytes, now);
      assert(current_file_);
      assert(!current_file_->bad());
      T_FILE_APPEND_POLICY::AppendToFile(*current_file_.get(), message);
      status_.appended_file_size += message_size_in_bytes;
      if (T_FINALIZE_POLICY::ShouldFinalize(status_, now)) {
        FinalizeCurrentFile();
      }
    }
  }

  // `ForceProcessing()` initiates processing of finalized files, if any.
  // It is most commonly used to resume processing due to an externla event
  // when it was suspended due to an `Unavailable` response from user processing code.
  // Real life scenario: User code returns `Unavailable` due to the device going offline,
  // all processing stops w/o retry policy being applied, `ForceProcessing()` is called
  // to resume processing on an external event of the device being back online.
  void ForceProcessing(bool force_finalize_current_file = false) {
    std::unique_lock<std::mutex> lock(status_mutex_);
    if (force_finalize_current_file || status_.finalized.queue.empty()) {
      if (current_file_) {
        FinalizeCurrentFile(lock);
      }
    }
    force_processing_ = true;
    queue_status_condition_variable_.notify_all();
  }

  // Removes all finalized and current files from disk.
  // USE CAREFULLY!
  void RemoveAllFSQFiles() const {
    for (const auto& file : ScanDir([this](const std::string& s, T_TIMESTAMP* t) {
           return T_FILE_NAMING_STRATEGY::finalized.ParseFileName(s, t);
         })) {
      T_FILE_SYSTEM::RemoveFile(file.full_path_name);
    }
    for (const auto& file : ScanDir([this](const std::string& s, T_TIMESTAMP* t) {
           return T_FILE_NAMING_STRATEGY::current.ParseFileName(s, t);
         })) {
      T_FILE_SYSTEM::RemoveFile(file.full_path_name);
    }
  }

 private:
  // If the current file exists, declare it finalized, rename it under a permanent name
  // and notify the worker thread that a new file is available.
  void FinalizeCurrentFile(std::unique_lock<std::mutex>& already_acquired_status_mutex_lock) {
    static_cast<void>(already_acquired_status_mutex_lock);
    if (current_file_) {
      current_file_.reset(nullptr);
      const std::string finalized_file_name =
          T_FILE_NAMING_STRATEGY::finalized.GenerateFileName(status_.appended_file_timestamp);
      FileInfo<T_TIMESTAMP> finalized_file_info(
          finalized_file_name,
          T_FILE_SYSTEM::JoinPath(working_directory_, finalized_file_name),
          status_.appended_file_timestamp,
          status_.appended_file_size);
      T_FILE_SYSTEM::RenameFile(current_file_name_, finalized_file_info.full_path_name);
      status_.finalized.queue.push_back(finalized_file_info);
      status_.appended_file_size = 0;
      status_.appended_file_timestamp = T_TIMESTAMP(0);
      current_file_name_.clear();
      queue_status_condition_variable_.notify_all();
    }
  }

  void FinalizeCurrentFile() {
    if (current_file_) {
      std::unique_lock<std::mutex> lock(status_mutex_);
      FinalizeCurrentFile(lock);
    }
  }

  // Scans the directory for the files that match certain predicate.
  // Gets their sized and and extracts timestamps from their names along the way.
  template <typename F>
  std::vector<FileInfo<T_TIMESTAMP>> ScanDir(F f) const {
    std::vector<FileInfo<T_TIMESTAMP>> matched_files_list;
    const auto& dir = working_directory_;
    T_FILE_SYSTEM::ScanDir(working_directory_,
                           [this, &matched_files_list, &f, dir](const std::string& file_name) {
      // if (T_FILE_NAMING_STRATEGY::template IsFinalizedFileName<T_TIMESTAMP>(file_name)) {
      T_TIMESTAMP timestamp;
      if (f(file_name, &timestamp)) {
        matched_files_list.emplace_back(file_name,
                                        T_FILE_SYSTEM::JoinPath(working_directory_, file_name),
                                        timestamp,
                                        T_FILE_SYSTEM::GetFileSize(T_FILE_SYSTEM::JoinPath(dir, file_name)));
      }
    });
    std::sort(matched_files_list.begin(), matched_files_list.end());
    return matched_files_list;
  }

  // ValidateCurrentFiles() expires the current file and/or creates the new one as necessary.
  void EnsureCurrentFileIsOpen(const uint64_t message_size_in_bytes, const T_TIMESTAMP now) {
    static_cast<void>(message_size_in_bytes);  // TODO(dkorolev): Call purge, use `message_size_in_bytes`.
    if (!current_file_) {
      current_file_name_ =
          T_FILE_SYSTEM::JoinPath(working_directory_, T_FILE_NAMING_STRATEGY::current.GenerateFileName(now));
      current_file_.reset(new typename T_FILE_SYSTEM::OutputFile(current_file_name_));
      status_.appended_file_timestamp = now;
    }
  }

  // The worker thread first scans the directory for present finalized and current files.
  // Present finalized files are queued up.
  // If more than one present current files is available, all but one are finalized on the spot.
  // The one remaining current file can be appended to or finalized depending on the strategy.
  void WorkerThread() {
    // Step 1/4: Get the list of finalized files.
    const std::vector<FileInfo<T_TIMESTAMP>>& finalized_files_on_disk =
        ScanDir([this](const std::string& s,
                       T_TIMESTAMP* t) { return T_FILE_NAMING_STRATEGY::finalized.ParseFileName(s, t); });
    status_.finalized.queue.assign(finalized_files_on_disk.begin(), finalized_files_on_disk.end());
    status_.finalized.total_size = 0;
    for (const auto& file : finalized_files_on_disk) {
      status_.finalized.total_size += file.size;
    }

    // Step 2/4: Get the list of current files.
    const std::vector<FileInfo<T_TIMESTAMP>>& current_files_on_disk = ScanDir([this](
        const std::string& s, T_TIMESTAMP* t) { return T_FILE_NAMING_STRATEGY::current.ParseFileName(s, t); });
    // TODO(dkorolev): Finalize all or all but one `current` files. Rename them and append them to the queue.
    static_cast<void>(current_files_on_disk);

    // Step 3/4: Signal that FSQ's status has been successfully parsed from disk and FSQ is ready to go.
    {
      std::unique_lock<std::mutex> lock(status_mutex_);
      status_ready_ = true;
      queue_status_condition_variable_.notify_all();
    }

    // Step 4/4: Start processing finalized files via T_PROCESSOR, respecting retry policy.
    while (true) {
      // Wait for a newly arrived file or another event to happen.
      std::unique_ptr<FileInfo<T_TIMESTAMP>> next_file;
      {
        std::unique_lock<std::mutex> lock(status_mutex_);
        auto predicate = [this]() {
          if (force_worker_thread_shutdown_) {
            return true;
          } else if (force_processing_) {
            return true;
          } else if (!status_.finalized.queue.empty()) {
            return true;
          } else {
            return false;
          }
        };
        if (!predicate()) {
          queue_status_condition_variable_.wait(lock, predicate);
        }
        if (force_worker_thread_shutdown_) {
          // TODO(dkorolev): Graceful shutdown logic.
          return;
        }
        if (!status_.finalized.queue.empty()) {
          next_file.reset(new FileInfo<T_TIMESTAMP>(status_.finalized.queue.front()));
        }
      }

      // Process the file, if available.
      if (next_file) {
        const FileProcessingResult result = processor_.OnFileReady(*next_file.get(), time_manager_.Now());
        static_cast<void>(result);  // TODO(dkorolev): Insert retry logic here.
        if (true) {
          std::unique_lock<std::mutex> lock(status_mutex_);
          assert(*next_file.get() == status_.finalized.queue.front());
          status_.finalized.queue.pop_front();
        }
      }
    }
  }

  Status status_;
  // Appending messages is single-threaded and thus lock-free.
  // The status of the processing queue, on the other hand, should be guarded.
  mutable std::mutex status_mutex_;
  // Set to true and pings the variable once the initial directory scan is completed.
  bool status_ready_ = false;
  mutable std::condition_variable queue_status_condition_variable_;

  T_PROCESSOR& processor_;
  std::string working_directory_;
  const T_TIME_MANAGER& time_manager_;
  const T_FILE_SYSTEM& file_system_;

  std::unique_ptr<typename T_FILE_SYSTEM::OutputFile> current_file_;
  std::string current_file_name_;

  std::thread worker_thread_;
  bool force_processing_ = false;
  bool force_worker_thread_shutdown_ = false;

  FSQ(const FSQ&) = delete;
  FSQ(FSQ&&) = delete;
  void operator=(const FSQ&) = delete;
  void operator=(FSQ&&) = delete;
};

}  // namespace fsq

#endif  // FSQ_H
