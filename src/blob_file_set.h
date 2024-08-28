#pragma once

#include <cstdint>

#include <atomic>
#include <unordered_map>
#include <unordered_set>

#include "db/log_reader.h"
#include "db/log_writer.h"
#include "port/port.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "util/mutexlock.h"

#include "blob_file_cache.h"
#include "blob_storage.h"
#include "titan/options.h"
#include "titan_stats.h"
#include "version_edit.h"

namespace rocksdb {
namespace titandb {

struct LogReporter : public log::Reader::Reporter {
  Status* status;
  void Corruption(size_t, const Status& s) override {
    if (status->ok()) *status = s;
  }
};

// BlobFileSet is the set of all the blobs file generated by Titan.
// It records blob file meta in terms of column family.
class BlobFileSet {
 public:
  explicit BlobFileSet(const TitanDBOptions& options, TitanStats* stats,
                       std::atomic<bool>* initialized, port::Mutex* mutex);

  // Sets up the storage specified in "options.dirname".
  // If the manifest doesn't exist, it will create one.
  // If the manifest exists, it will recover from the latest one.
  // It is a corruption if the persistent storage contains data
  // outside of the provided column families.
  Status Open(const std::map<uint32_t, TitanCFOptions>& column_families,
              const std::string& cache_prefix);

  // Applies *edit and saved to the manifest.
  // REQUIRES: mutex is held
  Status LogAndApply(VersionEdit& edit);

  // Adds some column families with the specified options.
  // REQUIRES: mutex is held
  void AddColumnFamilies(
      const std::map<uint32_t, TitanCFOptions>& column_families,
      const std::string& cache_prefix);

  // Drops some column families. The obsolete files will be deleted in
  // background when they will not be accessed anymore.
  // REQUIRES: mutex is held
  Status DropColumnFamilies(const std::vector<uint32_t>& handles,
                            SequenceNumber obsolete_sequence);

  // Destroy the column family. Only after this is called, the obsolete files
  // of the dropped column family can be physical deleted.
  // REQUIRES: mutex is held
  Status MaybeDestroyColumnFamily(uint32_t cf_id);

  // Logical deletes all the blobs within the ranges.
  // REQUIRES: mutex is held
  Status DeleteBlobFilesInRanges(uint32_t cf_id, const RangePtr* ranges,
                                 size_t n, bool include_end,
                                 SequenceNumber obsolete_sequence);

  // Allocates a new file number.
  uint64_t NewFileNumber() { return next_file_number_.fetch_add(1); }

  // REQUIRES: mutex is held
  std::weak_ptr<BlobStorage> GetBlobStorage(uint32_t cf_id) {
    auto it = column_families_.find(cf_id);
    if (it != column_families_.end()) {
      return it->second;
    }
    return std::weak_ptr<BlobStorage>();
  }

  // REQUIRES: mutex is held
  void GetObsoleteFiles(std::vector<std::string>* obsolete_files,
                        SequenceNumber oldest_sequence);

  // REQUIRES: mutex is held
  void GetAllFiles(std::vector<std::string>* files,
                   std::vector<VersionEdit>* edits);

  // REQUIRES: mutex is held
  bool IsColumnFamilyObsolete(uint32_t cf_id) {
    return obsolete_columns_.count(cf_id) > 0;
  }

  bool IsOpened() { return opened_.load(std::memory_order_acquire); }

  uint64_t GetBlockSize(uint32_t cf_id) {
    MutexLock l(mutex_);
    auto storage = GetBlobStorage(cf_id).lock();
    if (storage != nullptr && storage->cf_options().punch_hole_threshold > 0) {
      return storage->cf_options().block_size;
    }
    return 0;
  }

  std::unordered_map<uint64_t, uint64_t> GetFileBlockSizes(uint32_t cf_id) {
    MutexLock l(mutex_);
    auto storage = GetBlobStorage(cf_id).lock();
    return storage ? storage->GetFileBlockSizes()
                   : std::unordered_map<uint64_t, uint64_t>();
  }

 private:
  struct ManifestWriter;

  friend class BlobFileSizeCollectorTest;
  friend class VersionTest;

  Status Recover();

  Status OpenManifest(uint64_t number);

  Status WriteSnapshot(log::Writer* log);

  std::string dirname_;
  Env* env_;
  EnvOptions env_options_;
  TitanDBOptions db_options_;
  std::shared_ptr<Cache> file_cache_;

  TitanStats* stats_;
  port::Mutex* mutex_;

  // Indicate whether the gc initialization is finished.
  std::atomic<bool>* initialized_;
  // Indicate whether the blob file set Open is called.
  std::atomic<bool> opened_{false};

  std::vector<std::string> obsolete_manifests_;

  // As rocksdb described, `DropColumnFamilies()` only records the drop of the
  // column family specified by ColumnFamilyHandle. The actual data is not
  // deleted until the client calls `delete column_family`, namely
  // `DestroyColumnFamilyHandle()`. We can still continue using the column
  // family if we have outstanding ColumnFamilyHandle pointer. So here record
  // the dropped column family but the handler is not destroyed.
  std::unordered_set<uint32_t> obsolete_columns_;

  std::unordered_map<uint32_t, std::shared_ptr<BlobStorage>> column_families_;
  std::unique_ptr<log::Writer> manifest_;
  std::atomic<uint64_t> next_file_number_{1};
  uint64_t manifest_file_number_;

  std::deque<ManifestWriter*> manifest_writers_;
};

}  // namespace titandb
}  // namespace rocksdb
