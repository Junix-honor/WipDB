// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_set.h"

#include <algorithm>
#include <stdio.h>
#include <unordered_map>
#include <sstream>
#include <iomanip>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "kv/env.h"
#include "kv/table_builder.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/generator.h"
// #include "util/logging.h"

namespace kv {


static size_t TargetFileSize(const Options* options) {
  return options->max_file_size;
}

// Maximum bytes of overlaps in grandparent (i.e., level+2) before we
// stop building a single file in a level->level+1 compaction.
static int64_t MaxGrandParentOverlapBytes(const Options* options) {
  return 10 * TargetFileSize(options);
}

// Maximum number of bytes in all compacted files.  We avoid expanding
// the lower level file set of a compaction if it would make the
// total compaction cover more than this many bytes.
static int64_t ExpandedCompactionByteSizeLimit(const Options* options) {
  return 25 * TargetFileSize(options);
}

static double MaxBytesForLevel(const Options* options, int level) {
  // Note: the result for level zero is not really used since we set
  // the level-0 compaction threshold based on number of files.

  // Result for both level-0 and level-1
  double result = 10. * 1048576.0;
  while (level > 1) {
    result *= 10;
    level--;
  }
  return result;
}

static uint64_t MaxFileSizeForLevel(const Options* options, int level) {
  // We could vary per level to reduce number of files?
  return TargetFileSize(options);
}

static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) {
    sum += files[i]->file_size;
  }
  return sum;
}

Version::Version(VersionSet* vset)
      : vset_(vset), next_(this), prev_(this), refs_(0),
        file_to_compact_(nullptr),
        file_to_compact_level_(-1),
        compaction_score_(-1),
        compaction_level_(-1),
        should_split_(false){
          delay_compact_levels_ = std::vector<int>(8, 0);
          for (int & d : delay_compact_levels_) {
              d = random() % 6;
          }
}


Version::~Version() {
  assert(refs_ == 0);

  // Remove from linked list
  prev_->next_ = next_;
  next_->prev_ = prev_;

  // Drop references to files
  for (int level = 0; level < config::kNumLevels; level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      FileMetaData* f = files_[level][i];
      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) {
        delete f;
      }
    }
  }
}

int64_t Version::TotalSize() {
  int64_t sum = 0;
  for (auto& files : files_) {
    sum += TotalFileSize(files);
  }
  return sum;
}

int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files,
             const Slice& key) {
  uint32_t left = 0;
  uint32_t right = files.size();
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FileMetaData* f = files[mid];
    if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
      // Key at "mid.largest" is < "target".  Therefore all
      // files at or before "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "mid.largest" is >= "target".  Therefore all files
      // after "mid" are uninteresting.
      right = mid;
    }
  }
  return right;
}

static bool AfterFile(const Comparator* ucmp,
                      const Slice* user_key, const FileMetaData* f) {
  // null user_key occurs before all keys and is therefore never after *f
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

static bool BeforeFile(const Comparator* ucmp,
                       const Slice* user_key, const FileMetaData* f) {
  // null user_key occurs after all keys and is therefore never before *f
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

bool SomeFileOverlapsRange(
    const InternalKeyComparator& icmp,
    bool disjoint_sorted_files,
    const std::vector<FileMetaData*>& files,
    const Slice* smallest_user_key,
    const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    // Need to check against all files
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return true;  // Overlap
      }
    }
    return false;
  }

  // Binary search over file list
  uint32_t index = 0;
  if (smallest_user_key != nullptr) {
    // Find the earliest possible internal key for smallest_user_key
    InternalKey small(*smallest_user_key, kMaxSequenceNumber,kValueTypeForSeek);
    index = FindFile(icmp, files, small.Encode());
  }

  if (index >= files.size()) {
    // beginning of range is after all files, so no overlap.
    return false;
  }

  return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 16-byte value containing the file number and file size, both
// encoded using EncodeFixed64.
class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>* flist)
      : icmp_(icmp),
        flist_(flist),
        index_(flist->size()) {        // Marks as invalid
  }
  virtual bool Valid() const {
    return index_ < flist_->size();
  }
  virtual void Seek(const Slice& target) {
    index_ = FindFile(icmp_, *flist_, target);
  }
  virtual void SeekToFirst() { index_ = 0; }
  virtual void SeekToLast() {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  virtual void Next() {
    assert(Valid());
    index_++;
  }
  virtual void Prev() {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();  // Marks as invalid
    } else {
      index_--;
    }
  }
  Slice key() const {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }
  Slice value() const {
    assert(Valid());
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_+8, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  virtual Status status() const { return Status::OK(); }
 private:
  const InternalKeyComparator icmp_;
  const std::vector<FileMetaData*>* const flist_;
  uint32_t index_;

  // Backing store for value().  Holds the file number and size.
  mutable char value_buf_[16];
};

void Version::AddIteratorsKV(const ReadOptions& options,
                           std::vector<Iterator*>* iters) {
  // Merge all level files together since they may overlap
  for (int level = 0; level < config::kNumLevels; level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      iters->push_back(
          vset_->table_cache_->NewIteratorKV(
              options, files_[level][i]));
    }
  }
  
}

// Callback from TableCache::Get()
namespace {
enum SaverState {
  kNotFound,
  kFound,
  kDeleted,
  kCorrupt,
};
struct Saver {
  SaverState state;
  const Comparator* ucmp;
  Slice user_key;
  std::string* value;

  Status* status;
  const LookupKey* key;
  bool* found_final_value;  // Is value set correctly? Used by KeyMayExist
  MemTable* mem;
};
}
static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else {
    if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
      s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
      if (s->state == kFound) {
        s->value->assign(v.data(), v.size());
      }
    }
  }
}

static bool NewestFirst(FileMetaData* a, FileMetaData* b) {
  return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key,
                                 void* arg,
                                 bool (*func)(void*, int, FileMetaData*)) {
  // TODO(sanjay): Change Version::Get() to use this function.
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  // Search level-0 in order from newest to oldest.
  std::vector<FileMetaData*> tmp;
  tmp.reserve(files_[0].size());
  for (uint32_t i = 0; i < files_[0].size(); i++) {
    FileMetaData* f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      tmp.push_back(f);
    }
  }
  if (!tmp.empty()) {
    std::sort(tmp.begin(), tmp.end(), NewestFirst);
    for (uint32_t i = 0; i < tmp.size(); i++) {
      if (!(*func)(arg, 0, tmp[i])) {
        return;
      }
    }
  }

  // Search other levels.
  for (int level = 1; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    // Binary search to find earliest index whose largest key >= internal_key.
    uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
    if (index < num_files) {
      FileMetaData* f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
        // All of "f" is past any data for user_key
      } else {
        if (!(*func)(arg, level, f)) {
          return;
        }
      }
    }
  }
}

Status Version::GetKV(const ReadOptions& options,
                    const LookupKey& k,
                    std::string* value,
                    GetStats* stats) {
  // 根据 FileMetaData 来确定有 overlap 的 sstable，然后一层层的找。
  Slice ikey = k.internal_key();
  Slice user_key = k.user_key();
  const Comparator* ucmp = vset_->icmp_.user_comparator();
  Status s;

  stats->seek_file = nullptr;
  stats->seek_file_level = -1;
  FileMetaData* last_file_read = nullptr;
  int last_file_read_level = -1;

  // We can search level-by-level since entries never hop across
  // levels.  Therefore we are guaranteed that if we find data
  // in an smaller level, later levels are irrelevant.
  std::vector<FileMetaData*> tmp;

  for (int level = 0; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    // Log(Env::Default()->info_log_, "Level-%d has %d files",level, num_files);
    if (num_files == 0) continue;
    tmp.clear();
    // Get the list of files to search in this level
    FileMetaData* const* files = &files_[level][0];
    {
      // All sublevel files may overlap each other.  Find all files that
      // overlap user_key and process them in order from newest to oldest.
      tmp.reserve(num_files);
      for (uint32_t i = 0; i < num_files; i++) {
        FileMetaData* f = files[i];
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
            ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
          tmp.push_back(f);
        }
      }
      if (tmp.empty()) continue;

      // file overlap sstable in current level，search from new to old
      std::sort(tmp.begin(), tmp.end(), NewestFirst);
      files = &tmp[0];
      num_files = tmp.size();
    } 
    for (uint32_t i = 0; i < num_files; ++i) {
      if (last_file_read != nullptr && stats->seek_file == nullptr) {
        // We have had more than one seek for this read.  Charge the 1st file.
        stats->seek_file = last_file_read;
        stats->seek_file_level = last_file_read_level;
      }

      FileMetaData* f = files[i];
      last_file_read = f;
      last_file_read_level = level;

      Saver saver;
      saver.state = kNotFound;
      saver.ucmp = ucmp;
      saver.user_key = user_key;
      saver.value = value;
      s = vset_->table_cache_->GetKV(options, f,
                                   ikey, &saver, SaveValue);
      if (!s.ok()) {
        return s;
      }
      switch (saver.state) {
        case kNotFound:
          break;      // Keep searching in other files
        case kFound:
          return s;
        case kDeleted:
          s = Status::NotFound(Slice());  // Use empty error message for speed
          return s;
        case kCorrupt:
          s = Status::Corruption("corrupted key for ", user_key);
          return s;
      }
    }
  }

  return Status::NotFound(Slice());  // Use an empty error message for speed
}


bool Version::UpdateStats(const GetStats& stats) {
  FileMetaData* f = stats.seek_file;
  if (f != nullptr) {
    f->allowed_seeks--;
     if (f->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
      file_to_compact_ = f;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

bool Version::RecordReadSample(Slice internal_key) {
  ParsedInternalKey ikey;
  if (!ParseInternalKey(internal_key, &ikey)) {
    return false;
  }

  struct State {
    GetStats stats;  // Holds first matching file
    int matches;

    static bool Match(void* arg, int level, FileMetaData* f) {
      State* state = reinterpret_cast<State*>(arg);
      state->matches++;
      if (state->matches == 1) {
        // Remember first match.
        state->stats.seek_file = f;
        state->stats.seek_file_level = level;
      }
      // We can stop iterating once we have a second match.
      return state->matches < 2;
    }
  };

  State state;
  state.matches = 0;
  ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);

  // Must have at least two matches since we want to merge across
  // files. But what if we have a single file that contains many
  // overwrites and deletions?  Should we have another mechanism for
  // finding such files?
  if (state.matches >= 2) {
    // 1MB cost is about 1 seek (see comment in Builder::Apply).
    return UpdateStats(state.stats);
  }
  return false;
}

void Version::Ref() {
  ++refs_;
}

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}

bool Version::OverlapInLevel(int level,
                             const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}

int Version::PickLevelForMemTableOutput(
    const Slice& smallest_user_key,
    const Slice& largest_user_key) {
  int level = 0;
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
    // Push to next level if there is no overlap in next level,
    // and the #bytes overlapping in the level after that are limited.
    InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
    std::vector<FileMetaData*> overlaps;
    while (level < config::kMaxMemCompactLevel) {
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
        break;
      }
      if (level + 2 < config::kNumLevels) {
        // Check that file does not overlap too many grandparent bytes.
        GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
        const int64_t sum = TotalFileSize(overlaps);
        if (sum > MaxGrandParentOverlapBytes(vset_->options_)) {
          break;
        }
      }
      level++;
    }
  }
  return level;
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
void Version::GetOverlappingInputs(
    int level,
    const InternalKey* begin,
    const InternalKey* end,
    std::vector<FileMetaData*>* inputs) {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != nullptr) {
    user_begin = begin->user_key();
  }
  if (end != nullptr) {
    user_end = end->user_key();
  }
  const Comparator* user_cmp = vset_->icmp_.user_comparator();
  for (size_t i = 0; i < files_[level].size(); ) {
    FileMetaData* f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      inputs->push_back(f);
      if (level == 0) {
        // Level-0 files may overlap each other.  So check if the newly
        // added file has expanded the range.  If so, restart search.
        if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != nullptr && user_cmp->Compare(file_limit,
                                                       user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      }
    }
  }
}

std::string Version::DebugString() const {
  std::string r;
  r.append("\n");
  for (int level = 0; level < config::kNumLevels; level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' .. 'd']
    //   20:43['e' .. 'g']
    r.append("--- level ");
    AppendNumberTo(&r, level);
    r.append(" --- \n");
    const std::vector<FileMetaData*>& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.push_back(' ');
      AppendNumberTo(&r, files[i]->number);
      r.push_back(':');
      AppendNumberTo(&r, files[i]->file_size);
      r.append("[");
      r.append(files[i]->smallest.DebugString());
      r.append(" .. ");
      r.append(files[i]->largest.DebugString());
      r.append(" Level: ");
      AppendNumberTo(&r, files[i]->level);
      r.append(". Count: ");
      AppendNumberTo(&r, files[i]->count);
      r.append(". Median: ");
      r.append(files[i]->median.DebugString());
      r.append("\n");
    }
    
  }
  return r;
}


// A helper class so we can efficiently apply a whole sequence
// of edits to a particular state without creating intermediate
// Versions that contain full copies of the intermediate state.
class VersionSet::Builder {
 private:
  // Helper to sort by v->files_[file_number].smallest
  struct BySmallestKey {
    const InternalKeyComparator* internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      int r = internal_comparator->Compare(f1->smallest, f2->smallest);
      if (r != 0) {
        return (r < 0);
      } else {
        // Break ties by file number
        return (f1->number < f2->number);
      }
    }
  };

  typedef std::set<FileMetaData*, BySmallestKey> FileSet;
  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet* added_files;
  };

  VersionSet* vset_;
  Version* base_;
  LevelState levels_[config::kNumLevels];



 public:
  // Initialize a builder with the files from *base and other info from *vset
  Builder(VersionSet* vset, Version* base)
      : vset_(vset),
        base_(base)
        {
    base_->Ref();
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      levels_[level].added_files = new FileSet(cmp);
    }

  }

  ~Builder() {
    for (int level = 0; level < config::kNumLevels; level++) {
      const FileSet* added = levels_[level].added_files;
      std::vector<FileMetaData*> to_unref;
      to_unref.reserve(added->size());
      for (FileSet::const_iterator it = added->begin();
          it != added->end(); ++it) {
        to_unref.push_back(*it);
      }
      delete added;
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData* f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          delete f;
        }
      }
    }
    base_->Unref();
  }

  // Apply all of the edits in *edit to the current state.
  void Apply(VersionEdit* edit) {
    // Update compaction pointers
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
      const int level = edit->compact_pointers_[i].first;
      vset_->compact_pointer_[level] =
          edit->compact_pointers_[i].second.Encode().ToString();
    }

    // Delete files
    /*record the number of files to be deleted*/
    const VersionEdit::DeletedFileSet& del = edit->deleted_files_;
    for (VersionEdit::DeletedFileSet::const_iterator iter = del.begin();
         iter != del.end();
         ++iter) {
      const int level = iter->first;
      const uint64_t number = iter->second;
      levels_[level].deleted_files.insert(number);
    }

    // Add new files
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first;
      FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
      f->refs = 1;

      // We arrange to automatically compact this file after
      // a certain number of seeks.  Let's assume:
      //   (1) One seek costs 10ms
      //   (2) Writing or reading 1MB costs 10ms (100MB/s)
      //   (3) A compaction of 1MB does 25MB of IO:
      //         1MB read from this level
      //         10-12MB read from next level (boundaries may be misaligned)
      //         10-12MB written to next level
      // This implies that 25 seeks cost the same as the compaction
      // of 1MB of data.  I.e., one seek costs approximately the
      // same as the compaction of 40KB of data.  We are a little
      // conservative and allow approximately one seek for every 16KB
      // of data before triggering a compaction.
      f->allowed_seeks = (f->file_size / 16384);
      if (f->allowed_seeks < 100) f->allowed_seeks = 100;

      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files->insert(f);
    }
  }


  // Save the current state in *v.
  void SaveTo(Version* v) {
    BySmallestKey cmp;
    // compare the smallest key 
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.

      // files_ are sortted according to smallest key
      const std::vector<FileMetaData*>& base_files = base_->files_[level];
      std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
      std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
      const FileSet* added = levels_[level].added_files;
  
      v->files_[level].reserve(base_files.size() + added->size());
      // iterate every new added files，FileSet is sorted accroding to smallest key 
      for (FileSet::const_iterator added_iter = added->begin();
           added_iter != added->end();
           ++added_iter) {
        // Add all smaller files listed in base_
        // sort according to smallest key 
        for (std::vector<FileMetaData*>::const_iterator bpos
                 = std::upper_bound(base_iter, base_end, *added_iter, cmp);
             base_iter != bpos;
             ++base_iter) {
          MaybeAddFile(v, level, *base_iter);
        }

        MaybeAddFile(v, level, *added_iter);
      }

      // Add remaining base files
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(v, level, *base_iter);
      }
    }


  }

  void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {
      // File is deleted: do nothing
    } else {
      std::vector<FileMetaData*>* files = &v->files_[level];    
      f->refs++;
      files->push_back(f);
    }
  }
};

VersionSet::VersionSet(const std::string& dbname,
                       const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      next_level_file_number_(20),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      // descriptor_file_(nullptr),
      // descriptor_log_(nullptr),
      dummy_versions_(this),
      current_(nullptr) {
  AppendVersion(new Version(this));
  split_delay_ = 4 * random_double();
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);  // List must be empty
  // delete descriptor_log_;
  // delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
  // append new version (v) to version list，and set current as v

  // Make "v" current
  assert(v->refs_ == 0);
  assert(v != current_);
  if (current_ != nullptr) {
    current_->Unref();
  }
  current_ = v;
  v->Ref();

  // Append to linked list
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

std::string Version::PrintDelay(){
  std::string info = "Delay info: [";
  for (auto d : delay_compact_levels_) {
    info += std::to_string(d) + ",";
  }
  info += "]";
  return info;
}

void Version::RandomizeDelay() {
    for (int i = 0; i < config::kNumLevels; i++) {
      delay_compact_levels_[i] = random() % 6;
    }
}

Status VersionSet::LogAndApplyKV(VersionEdit* edit, port::Mutex* mu) {
  // After compaction this function will be called. 
  // Use VersionEdit to generate a new version

  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }

  if (!edit->has_prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  Version* v = new Version(this);
  {

    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }

  v->SetDelay(current_->delay_compact_levels_); // copy old delays
  
  FinalizeKV(v);

  // Initialize new descriptor log file if necessary by creating
  // a temporary file that contains a snapshot of the current version.
  std::string new_manifest_file;
  Status s;

  // Install the new version
  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {
    delete v;
  }

  return s;
}

Status VersionSet::Recover(bool *save_manifest) {
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      if (this->status->ok()) *this->status = s;
    }
  };

  // Read "CURRENT" file, which contains a pointer to the current manifest file
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current[current.size()-1] != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  current.resize(current.size() - 1);

  // current save manifest filename：MANIFEST-000002. 
  std::string dscname = dbname_ + "/" + current;
  SequentialFile* file;
  // open current manifest file
  s = env_->NewSequentialFile(dscname, &file);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      return Status::Corruption(
            "CURRENT points to a non-existent file", s.ToString());
    }
    return s;
  }

  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;

  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;

  Builder builder(this, current_);

  {
    LogReporter reporter;
    reporter.status = &s;
    // manifest is written using log:Writer. We use relative reader to read.
    log::Reader reader(file, &reporter, true/*checksum*/, 0/*initial_offset*/);
    Slice record;
    std::string scratch;
    // replay all the edits
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (s.ok()) {
        if (edit.has_comparator_ &&
            edit.comparator_ != icmp_.user_comparator()->Name()) {
          s = Status::InvalidArgument(
              edit.comparator_ + " does not match existing comparator ",
              icmp_.user_comparator()->Name());
        }
      }

      if (s.ok()) {
        builder.Apply(&edit);
      }

      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }

      if (edit.has_prev_log_number_) {
        prev_log_number = edit.prev_log_number_;
        have_prev_log_number = true;
      }

      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }

      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }

    }
  }
  delete file; // close manifest file
  file = nullptr;

  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption("no meta-nextfile entry in descriptor");
    } else if (!have_last_sequence) {
      s = Status::Corruption("no last-sequence-number entry in descriptor");
    } 

    if (!have_prev_log_number) {
      prev_log_number = 0;
    }

    MarkFileNumberUsed(prev_log_number);
    MarkFileNumberUsed(log_number);
    
  }

  if (s.ok()) {
    Version* v = new Version(this);

    // replay all the applied edits, and get the final version. save to v
    builder.SaveTo(v);

    // Install recovered version
    Finalize(v);

    AppendVersion(v);
    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    prev_log_number_ = prev_log_number;

    // See if we can reuse the existing MANIFEST file.
    if (ReuseManifest(dscname, current)) {
      // No need to save new manifest
    } else {
      *save_manifest = true;
    }
  }

  return s;
}

bool VersionSet::ReuseManifest(const std::string& dscname,
                               const std::string& dscbase) {
  if (!options_->reuse_logs) {
    return false;
  }
  
  FileType manifest_type;
  uint64_t manifest_number;
  uint64_t manifest_size;
  if (!ParseFileName(dscbase, &manifest_number, &manifest_type) ||
      manifest_type != kDescriptorFile ||
      !env_->GetFileSize(dscname, &manifest_size).ok() ||
      // Make new compacted MANIFEST if old one is too big
      manifest_size >= TargetFileSize(options_)) {
    return false;
  }

  manifest_file_number_ = manifest_number;
  return true;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

void VersionSet::MarkLevelFileNumberUsed(uint64_t number) {
  if (next_level_file_number_ <= number) {
    next_level_file_number_ = number + 1;
  }
}
void VersionSet::Finalize(Version* v) {
  // Precomputed best level for next compaction
  // pick the largest core level as next compaction level
  int best_level = -1;
  double best_score = -1;

  for (int level = 0; level < config::kNumLevels-1; level++) {
    double score;
    if (level == 0) {
      // We treat level-0 specially by bounding the number of files
      // instead of number of bytes for two reasons:
      //
      // (1) With larger write-buffer sizes, it is nice not to do too
      // many level-0 compactions.
      //
      // (2) The files in level-0 are merged on every read and
      // therefore we wish to avoid too many files when the individual
      // file size is small (perhaps because of a small write-buffer
      // setting, or very high compression ratios, or lots of
      // overwrites/deletions).
      score = v->files_[level].size() /
          static_cast<double>(config::kL0_CompactionTrigger);
    } else {
      // Compute the ratio of current size to size limit.
      const uint64_t level_bytes = TotalFileSize(v->files_[level]);
      score =
          static_cast<double>(level_bytes) / MaxBytesForLevel(options_, level);
    }

    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }

  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}


void VersionSet::FinalizeKV(Version* v) {
  // Precomputed best level for next compaction
  int best_level = -1;
  double best_score = -1;

  v->compaction_levels_.clear();

  // major compaction only happens at [0, kSplitLevel-1]
  
  for (int level = kSplitLevel - 1; level >= 0; level--) {
    if(v->files_[level].size() >= config::kKV_Level_Ratio + v->delay_compact_levels_[level]) { // 决定 compaction 的时机，随机延迟
      best_level = level;
      best_score = 2;
      v->compaction_levels_.insert(level);
      break;
    }
  }

  // if file number is more than 8 + a random number
  if (v->files_[kSplitLevel].size() >= 8  + split_delay_ ) {
    v->should_split_ = true;
  }
  
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}


Status VersionSet::WriteSnapshot(log::Writer* log) {
  // generate a log record of current version and save to manifest file

  // Save metadata
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());

  // Save compaction pointers
  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  // Save files
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest, f->median, f->count);
    }
  }

  edit.SetNextFile(next_file_number_);
  edit.SetLastSequence(last_sequence_);
  std::string record;
  edit.EncodeTo(&record);
  Log(options_->info_log, "Snapshot save %s: %s\n", dbname_.c_str(), edit.DebugString().c_str());
  return log->AddRecord(record);
}

int VersionSet::NumLevelFiles(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return current_->files_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
  // Update code if kNumLevels changes
  assert(config::kNumLevels == 7);
  snprintf(scratch->buffer, sizeof(scratch->buffer),
           "files[ %d %d %d %d %d %d %d ]",
           int(current_->files_[0].size()),
           int(current_->files_[1].size()),
           int(current_->files_[2].size()),
           int(current_->files_[3].size()),
           int(current_->files_[4].size()),
           int(current_->files_[5].size()),
           int(current_->files_[6].size()));
  return scratch->buffer;
}

uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = v->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
        // Entire file is before "ikey", so just add the file size
        result += files[i]->file_size;
      } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
        // Entire file is after "ikey", so ignore
        if (level > 0) {
          // Files other than level 0 are sorted by meta->smallest, so
          // no further files in this level will contain data for
          // "ikey".
          break;
        }
      } else {
        // "ikey" falls in the range for this table.  Add the
        // approximate offset of "ikey" within the table.
        Table* tableptr;
        Iterator* iter = table_cache_->NewIteratorKV(
            ReadOptions(), files[i], &tableptr);
        if (tableptr != nullptr) {
          result += tableptr->ApproximateOffsetOf(ikey.Encode());
        }
        delete iter;
      }
    }
  }
  return result;
}


void VersionSet::AddLiveTables(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_;
       v != &dummy_versions_;
       v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

void VersionSet::AddLiveFilesKV(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_;
       v != &dummy_versions_;
       v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

int64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return TotalFileSize(current_->files_[level]);
}

int64_t VersionSet::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<FileMetaData*> overlaps;
  for (int level = 1; level < config::kNumLevels - 1; level++) {
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      const FileMetaData* f = current_->files_[level][i];
      current_->GetOverlappingInputs(level+1, &f->smallest, &f->largest,
                                     &overlaps);
      const int64_t sum = TotalFileSize(overlaps);
      if (sum > result) {
        result = sum;
      }
    }
  }
  return result;
}

// Stores the minimal range that covers all entries in inputs in
// *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs,
                          InternalKey* smallest,
                          InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

// Stores the minimal range that covers all entries in inputs1 and inputs2
// in *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2,
                           InternalKey* smallest,
                           InternalKey* largest) {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

std::string VersionSet::MedianOfSplitLevelFiles() {
    Version* v = current_;
    std::vector<std::string> res;
  
    for (int i = 0; i <= kSplitLevel; ++i) {
      for (int j = 0; j < v->files_[i].size(); ++j) {
          res.push_back(v->files_[i][j]->median.user_key().ToString());
      }
    }
    
    
    sort(res.begin(), res.end());
    int split_pos = std::max((int)(res.size() - 1) / 2, 0);
    return res[split_pos];
  }

std::string VersionSet::StrAdd(const std::string& sa, uint64_t b) {
    std::string sb = std::to_string(b);
    assert(sa.size() >= sb.size());
    int alen = sa.size();
    std::stringstream ss;
    ss << std::setw(alen) << std::setfill('0') << std::stol(sa) + b;
    return ss.str();
}

std::vector<std::string> VersionSet::GeneratePivots(int T) {
    Version* v = current_;
    std::vector<std::string> median;
    std::vector<std::string> smallest;
    std::vector<std::string> largest;
  
    for (int i = 0; i <= kSplitLevel; ++i) {
        for (int j = 0; j < v->files_[i].size(); ++j) {
            median.push_back(v->files_[i][j]->median.user_key().ToString());
            smallest.push_back(v->files_[i][j]->smallest.user_key().ToString());
            largest.push_back(v->files_[i][j]->largest.user_key().ToString());
        }
    }
    
    
    sort(smallest.begin(), smallest.end());
    sort(largest.begin(), largest.end());
    sort(median.begin(), median.end());

    int split_pos = std::max((int)(median.size() - 1) / 2, 0);
    long m = std::stol(median[split_pos]);
    long s = std::stol(smallest.front());
    long l = std::stol(largest.back());

    std::vector<std::string> pivots;
    uint64_t interval = (l - s) / T;
    for (int i = 1; i < T; ++i) {
        pivots.push_back(StrAdd(smallest.front(), interval * i));
    }
    return pivots;
  }

Iterator* VersionSet::MakeInputIteratorKV(Compaction* c, uint64_t& total_count) {
  // build iterator for all the tables 
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;
  total_count = 0;
  // Level-0 files have to be merged together.  For other levels,
  // we will make a concatenating iterator per level.
  // TODO(opt): use concatenating iterator for level-0 if there is no overlap
  const int space = c->inputs_[0].size() + 1;
  Iterator** list = new Iterator*[space];
  int num = 0;

  // all table need to be merged is stored in inputs_[0]
  // this is done in PickCompactionKV()
  const std::vector<FileMetaData*>& files = c->inputs_[0];
  for (size_t i = 0; i < files.size(); i++) {
    total_count += files[i]->count;
    list[num++] = table_cache_->NewIteratorKV(options, files[i]);
  }
  
  assert(num <= space); 
  Iterator* result = NewMergingIterator(&icmp_, list, num);
  delete[] list;
  return result;
}



Compaction* VersionSet::PickCompactionKVAtLevel(int level) {
  Compaction* c;
  if(level < 0) return nullptr;
  assert(level >= 0);
  assert(level+1 < config::kNumLevels);
  c = new Compaction(options_, level);

  for (size_t i = 0; i < current_->files_[level].size(); i++) {
      FileMetaData* f = current_->files_[level][i];
      c->inputs_[0].push_back(f);
      // if (c->inputs_[0].size() >= 8) break;       
  }

  c->input_version_ = current_;
  c->input_version_->Ref();
  
  return c;
}


Compaction* VersionSet::PickCompactionKVForQuery() {
  Compaction* c;

  // pick all the table at 0 to kSplitLevel Level
  int level = config::kSplitLevel;
  for(int i = config::kSplitLevel; i >= 0; --i) {
    if (current_->files_[i].size() != 0) {
        level = i;
        break;
    }
  }
  level = std::min(level, kSplitLevel); 
  if(level < 0) return nullptr;
  assert(level >= 0);
  assert(level+1 < config::kNumLevels);

  // Major compaction all the sstable to the last level
  c = new Compaction(options_, level);
  for (int l = 0; l <= level; ++l) {
    for (size_t i = 0; i < current_->files_[l].size(); i++) {
      FileMetaData* f = current_->files_[l][i];
      c->inputs_[0].push_back(f); 
    }
  }
  
  // if the number of all sstable is smaller than 4,
  // we do not do compaction.
  if (c->inputs_[0].size() < 4) {
    delete c;
    return nullptr;
  }

  c->input_version_ = current_;
  c->input_version_->Ref();
  
  return c;
}


Compaction* VersionSet::PickCompactionForSplit() {
  // pick all tables
  Compaction* c;
  c = new Compaction(options_, kSplitLevel - 1); // kSplitLevel - 1 means we want to merge all tables to kSplitLevel
  for (int l = 0; l < config::kNumLevels; ++l) {
    for (size_t i = 0; i < current_->files_[l].size(); i++) {
      FileMetaData* f = current_->files_[l][i];
      c->inputs_[0].push_back(f); 
    }
  }
  
  Log(options_->info_log, "=== Split Compaction %d files %s\n", (int)c->inputs_[0].size(), current_->DebugString().c_str());
  c->input_version_ = current_;
  c->input_version_->Ref();
  return c;
}

Compaction* VersionSet::ManualPickCompactionKV() {
  // pick all tables
  Compaction* c;

  int level = config::kNumLevels - 1;
  for(int i = config::kNumLevels - 1; i >= 0; --i) {
    if (current_->files_[i].size() != 0) {
        level = i;
        break;
    }
  }
  level = std::min(level, kSplitLevel - 1); // at most to split level
  if(level < 0) return nullptr;
  assert(level >= 0);
  assert(level+1 < config::kNumLevels);

  c = new Compaction(options_, level);
  for (int l = 0; l <= level; ++l) {
    for (size_t i = 0; i < current_->files_[l].size(); i++) {
      FileMetaData* f = current_->files_[l][i];
      c->inputs_[0].push_back(f); 
    }
  }
  
  Log(options_->info_log, "=== Manual Compaction %d files %s\n", (int)c->inputs_[0].size(), current_->DebugString().c_str());
  
  c->input_version_ = current_;
  c->input_version_->Ref();
  
  return c;
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest, largest;
  GetRange(c->inputs_[0], &smallest, &largest);

  current_->GetOverlappingInputs(level+1, &smallest, &largest, &c->inputs_[1]);

  // Get entire range covered by compaction
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  // See if we can grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up.
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
    const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const int64_t expanded0_size = TotalFileSize(expanded0);
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size <
            ExpandedCompactionByteSizeLimit(options_)) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      current_->GetOverlappingInputs(level+1, &new_start, &new_limit,
                                     &expanded1);
      if (expanded1.size() == c->inputs_[1].size()) {
        Log(options_->info_log,
            "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
            level,
            int(c->inputs_[0].size()),
            int(c->inputs_[1].size()),
            long(inputs0_size), long(inputs1_size),
            int(expanded0.size()),
            int(expanded1.size()),
            long(expanded0_size), long(inputs1_size));
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                   &c->grandparents_);
  }

  // Update the place where we will do the next compaction for this level.
  // We update this immediately instead of waiting for the VersionEdit
  // to be applied so that if the compaction fails, we will try a different
  // key range next time.
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}

Compaction* VersionSet::CompactRange(
    int level,
    const InternalKey* begin,
    const InternalKey* end) {
  std::vector<FileMetaData*> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) {
    return nullptr;
  }

  // Avoid compacting too much in one shot in case the range is large.
  // But we cannot do this for level-0 since level-0 files can overlap
  // and we must not pick one file and drop another older file if the
  // two files overlap.
  if (level > 0) {
    const uint64_t limit = MaxFileSizeForLevel(options_, level);
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
      uint64_t s = inputs[i]->file_size;
      total += s;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }

  Compaction* c = new Compaction(options_, level);
  c->input_version_ = current_;
  c->input_version_->Ref();
  c->inputs_[0] = inputs;
  SetupOtherInputs(c);
  return c;
}

Compaction::Compaction(const Options* options, int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(options, level)),
      input_version_(nullptr),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0) {
  for (int i = 0; i < config::kNumLevels; i++) {
    level_ptrs_[i] = 0;
  }
}

Compaction::~Compaction() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
  }
}

bool Compaction::IsTrivialMove() const {
  const VersionSet* vset = input_version_->vset_;
  // Avoid a move if there is lots of overlapping grandparent data.
  // Otherwise, the move could create a parent file that will require
  // a very expensive merge later on.
  return (num_input_files(0) == 1 && num_input_files(1) == 0 &&
          TotalFileSize(grandparents_) <=
              MaxGrandParentOverlapBytes(vset->options_));
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      edit->DeleteFile(inputs_[which][i]->level, inputs_[which][i]->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // Maybe use binary search to find right entry instead of linear search?
  const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
    for (; level_ptrs_[lvl] < files.size(); ) {
      FileMetaData* f = files[level_ptrs_[lvl]];
      if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // We've advanced far enough
        if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          // Key falls in this file's range, so definitely not base level
          return false;
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key) {
  const VersionSet* vset = input_version_->vset_;
  // Scan to find earliest grandparent file that contains key.
  const InternalKeyComparator* icmp = &vset->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
      icmp->Compare(internal_key,
                    grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  seen_key_ = true;

  if (overlapped_bytes_ > MaxGrandParentOverlapBytes(vset->options_)) {
    // Too much overlap for current output; start new output
    overlapped_bytes_ = 0;
    return true;
  } else {
    return false;
  }
}

void Compaction::ReleaseInputs() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
    input_version_ = nullptr;
  }
}

}  // namespace leveldb
