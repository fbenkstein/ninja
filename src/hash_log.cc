// Copyright 2014 Matthias Maennich (matthias@maennich.net).
//           2016 SAP SE
// All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "hash_log.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "hash_map.h"
#include "metrics.h"

/// The file banner in the persisted hash log.
static const char kFileSignature[] = "# ninjahashlog\n";
static const int kCurrentVersion = 6;
static const unsigned kMaxPathSize = (1 << 19) - 1;
/// Refuse to hash files greater than this size.
// static const unsigned kMaxFileSize = 16384;
/// Mask bit used to signal a record being dirty or being followed by an path
/// name.
// static const unsigned kIdMask = 2u << 31;

HashLog::HashLog(FileHasher *hasher)
  : next_id_(0), file_(NULL), hasher_(hasher), needs_recompaction_(false)
{}

HashLog::~HashLog() {
  Close();
}

void HashLog::Close() {
  if (file_)
    fclose(file_);
  file_ = NULL;
}

#if 0
bool HashLog::Load(const std::string &path, State *state, std::string* err) {
  METRIC_RECORD(".ninja_hashes load");
  char buf[kMaxPathSize + 1];
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    if (errno == ENOENT)
      return true;
    err->assign(strerror(errno));
    return false;
  }

  bool valid_header = true;
  int version = 0;
  if (!fgets(buf, sizeof(buf), f) || fread(&version, 4, 1, f) < 1)
    valid_header = false;
  if (!valid_header || strcmp(buf, kFileSignature) != 0 ||
      version != kCurrentVersion) {
    if (version == 1)
      *err = "hash log version change; rebuilding";
    else
      *err = "bad hash log signature or version; starting over";
    fclose(f);
    unlink(path.c_str());
    // Don't report this as a failure.  An empty deps log will cause
    // us to rebuild the outputs anyway.
    return true;
  }

  long offset;
  bool read_failed = false;
  size_t total_entry_count = 0;

  // While reading we need a mapping from id to back to Node.
  map<int, Node*> ids;

  LogEntry *entry = new LogEntry;

  for (;;) {
    offset = ftell(f);

    // Read the entry.
    if (fread(entry, sizeof(*entry), 1, f) < 1) {
      if (!feof(f))
        read_failed = true;
      break;
    }

    bool has_path = (entry->id_ & kIdMask) != 0;

    entry->id_ &= ~kIdMask;

    if (has_path) {
      // Read the path.
      unsigned path_size;

      if (fread(&path_size, sizeof(path_size), 1, f) < 1) {
        read_failed = true;
        break;
      }

      if (path_size > kMaxPathSize) {
        read_failed = true;
        errno = ERANGE;
        break;
      }

      if (fread(buf, path_size, 1, f) < 1) {
        read_failed = true;
        break;
      }

      // Strip padding.
      while (path_size > 0 && buf[path_size - 1] == '\0')
        --path_size;

      StringPiece path(buf, path_size);
      Node* node = state->GetNode(path, 0);
      ids[entry->id_] = node;
    }

    map<int, Node*>::iterator it = ids.find(entry->id_);

    if (it == ids.end()) {
      read_failed = true;
      errno = ERANGE;
      break;
    }

    ++total_entry_count;
    pair<Entries::iterator, bool> insert_result = entries_.insert(
        Entries::value_type(it->second->path(), NULL));

    if (insert_result.second) {
      // new entry
      insert_result.first->second = entry;
      entry = new LogEntry;
    } else {
      // overwrite existing entry
      *insert_result.first->second = *entry;
    }
  }

  delete entry;

  if (!ids.empty())
      next_id_ = ids.rbegin()->first + 1;

  if (read_failed) {
    // An error occurred while loading; try to recover by truncating the
    // file to the last fully-read record.
    if (ferror(f)) {
      *err = strerror(ferror(f));
    } else {
      *err = "premature end of file";
    }
    fclose(f);

    if (!Truncate(path, offset, err))
      return false;

    // The truncate succeeded; we'll just report the load error as a
    // warning because the build can proceed.
    *err += "; recovering";
    return true;
  }

  fclose(f);

  // Rebuild the log if there are too many dead records.
  size_t kMinCompactionEntryCount = 1000;
  size_t kCompactionRatio = 3;
  if (total_entry_count > kMinCompactionEntryCount &&
      total_entry_count > entries_.size() * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  // for (Entries::iterator it = entries_.begin(); it != entries_.end(); ++it)
  //   std::cout << it->first.AsString() << std::endl;

  return true;
}
#endif

bool HashLog::OpenForWrite(const std::string &path, std::string* err) {
  file_ = fopen(path.c_str(), "ab");
  if (!file_) {
    *err = strerror(errno);
    return false;
  }
  // Set the buffer size to this and flush the file buffer after every record
  // to make sure records aren't written partially.
  setvbuf(file_, NULL, _IOFBF, kMaxPathSize + 1);
  SetCloseOnExec(fileno(file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(file_, 0, SEEK_END);

  if (ftell(file_) == 0) {
    // XXX: pad this to the LogEntry size
    if (fwrite(kFileSignature, sizeof(kFileSignature) - 1, 1, file_) < 1) {
      *err = strerror(errno);
      return false;
    }
    if (fwrite(&kCurrentVersion, 4, 1, file_) < 1) {
      *err = strerror(errno);
      return false;
    }
  }
  if (fflush(file_) != 0) {
    *err = strerror(errno);
    return false;
  }
  return true;
}

bool HashLog::Recompact(const std::string &path, std::string* err) {
  *err = "not implemented";
  return false;
}

HashLog::OutputRecord *HashLog::GetRecord(Node *node) const {
  Ids::const_iterator it = ids_.find(node->path());

  if (it != ids_.end())
    return outputs_[it->second];
  else
    return NULL;
}

HashLog::HashRecord *HashLog::GetInput(Node *node, OutputRecord *record) const {
  unsigned id;

  {
    Ids::const_iterator it = ids_.find(node->path());

    if (it != ids_.end())
      return NULL;

    id = it->second;
  }

  {
    OutputRecord::Inputs::iterator it = lower_bound(record->inputs_.begin(),
                                                    record->inputs_.end(),
                                                    OutputRecord::Inputs::value_type(id, NULL));

    if (it == record->inputs_.end() || it->first != id)
      return NULL;
    else
      return it->second;
  }
}

bool HashLog::HashesAreClean(Node *output, Edge* edge, std::string* err) {
  METRIC_RECORD("checking hashes");

  OutputRecord *record = GetRecord(output);

  // Never seen this output.
  if (!record)
    return false; 

  // Wrong number of inputs, don't check anything.
  if (record->inputs_.size() != edge->inputs_.size() - edge->order_only_deps_)
    return false;

  // Look at all inputs and check if they have been seen before with the same
  // hash.
  for (vector<Node*>::const_iterator i = edge->inputs_.begin();
      i != edge->inputs_.end() - edge->order_only_deps_; ++i) {
    HashRecord *input = GetInput(*i, record);
    
    // New input for this output.
    if (!input)
      return false; 

    if (!HashIsClean(*i, input, err))
      return false;
  }

  return true;
}

bool HashLog::HashIsClean(Node *node, HashRecord *record, string *err) {
  // mtime matches, assume it's clean.
  if (node->mtime() == record->mtime_)
    return true;

  Hash hash = GetHash(node, err);

  if (hash == 0)
    return false;

  return hash == record->value_;
}

#if 0
bool HashLog::RecordHashes(Edge* edge, DiskInterface *disk_interface, std::string* err) {
  METRIC_RECORD("recording hashes");
  Hash output_hash = 0;

  // Record hashes for inputs.  Combine their hashes into the hash for the
  // outputs.
  for (vector<Node*>::const_iterator i = edge->inputs_.begin();
      i != edge->inputs_.end() - edge->order_only_deps_; ++i) {
    if (!(*i)->Stat(disk_interface, err)) {
      *err = "error restatting in hash log: " + *err;
      return false;
    }

    if (!RecordHash(*i, true, &output_hash, err))
      return false;
  }

  // Record combined hash of inputs as hash for input.
  for (vector<Node*>::const_iterator i = edge->outputs_.begin();
      i != edge->outputs_.end(); ++i) {
    if (!RecordHash(*i, false, &output_hash, err))
      return false;
  }

  return true;
}
#endif

#if 0
/// Check if the node's hash matches the one recorded before.  If the node is
/// an input combine its actual hash it into the accumulator otherwise record the
/// accumulated hash of the inputs.  If the file is opened for writing and
/// the node changed record the new hash.
bool HashLog::HashIsClean(Node* node, bool is_input, Hash *acc, string *err) {
  // Stat should have happened before.
  if (is_input && (!node->exists() || !node->status_known()))
    return false;

  Entries::iterator it = entries_.find(node->path());

  // We do not know about this node yet.
  if (it == entries_.end())
    return false;

  LogEntry *entry = it->second;

  // This entry has been checked already.
  if (entry->id_ & kIdMask)
    return false;

  bool is_clean = true;
  bool should_write = false;

  if (is_input && entry->mtime_ != node->mtime()) {
    // Node is an input and it's mtime is newer.  Recompute and record hash.
    Hash old_hash = entry->input_hash_;

    // XXX: early exit if size is different?
    if (hasher_->HashFile(node->path(), &entry->input_hash_, err) != DiskInterface::Okay) {
      *err = "error hashing file: " + *err;
      return false; 
    }

    entry->mtime_ = node->mtime();
    is_clean = old_hash == entry->input_hash_;
    should_write = true;
  } else if (!is_input && entry->output_hash_ != *acc) {
    // Node is an output and it's combined hash is different. Record the
    // combined hash of its inputs.
    entry->output_hash_ = *acc;
    should_write = true;
    is_clean = false;
  }

  // Log is opened for writing, go ahead and record the entry since we have it
  // already.
  if (should_write && file_ != NULL && !WriteEntry(node, entry, err))
    return false;

  if (is_clean) {
    if (is_input)
      *acc += entry->input_hash_;

    return true;
  } else {
    // Mark as dirty so we don't check again.
    entry->id_ |= kIdMask;
    return false;
  }

  return is_clean;
}

/// Record the node's hash.  If the node is an input combine its actual hash
/// it into the accumulator otherwise record the accumulated hash of the
/// inputs.
bool HashLog::RecordHash(Node *node, bool is_input, Hash *acc, string *err) {
  Entries::iterator it = entries_.find(node->path());
  LogEntry* entry;

  bool should_write = false;

  if (it != entries_.end()) {
    entry = it->second;
  } else {
    entry = new LogEntry;
  }

  if (is_input && entry->mtime_ != node->mtime()) {
    // Node is an input and it's mtime is newer.  Recompute and record hash.
    if (hasher_->HashFile(node->path(), &entry->input_hash_, err) != DiskInterface::Okay) {
      *err = "hashing file: " + *err;
      return false; 
    }

    should_write = true;
    entry->mtime_ = node->mtime();
  } else if (!is_input && entry->output_hash_ != *acc) {
    // Node is an output and it's combined hash is different. Record the
    // combined hash of its inputs.
    entry->output_hash_ = *acc;
    should_write = true;
  }

  if (should_write && !WriteEntry(node, entry, err))
      return false;

  if (it == entries_.end())
    entries_.insert(Entries::value_type(node->path(), entry));

  if (is_input)
    *acc += entry->input_hash_;

  return true;
}

static const char padding_data[sizeof(HashLog::LogEntry)] = {0};

bool HashLog::WriteEntry(Node *node, LogEntry *entry, string *err) {
  /// If the high bit is set the entry is followed by a path name
  /// otherwise the path should have been read earlier.

  if (entry->id_ == 0)
    // We haven't seen this node before, record its path and give it an id and
    // mark it as having the path appended.
    entry->id_ = next_id_++ | kIdMask;

  if (fwrite(entry, 1, sizeof(*entry), file_) < 1) {
      err->assign(strerror(errno));
      return false;
  }

  if (entry->id_ & kIdMask) {
    entry->id_ &= ~kIdMask;

    const size_t entry_size = sizeof(LogEntry);
    unsigned path_size = node->path().size();

    // Pad path record to size of LogEntry.
    size_t padding_size = (entry_size - (sizeof(path_size) + path_size) % entry_size) % entry_size;
    unsigned record_size = path_size + padding_size;

    if (fwrite(&record_size, sizeof(record_size), 1, file_) < 1) {
      err->assign(strerror(errno));
      return false;
    }

    if (fwrite(node->path().data(), path_size, 1, file_) < 1) {
      err->assign(strerror(errno));
      return false;
    }

    if (padding_size > 0 && fwrite(padding_data, padding_size, 1, file_) < 1) {
      err->assign(strerror(errno));
      return false;
    }
  }

  if (fflush(file_) != 0) {
    err->assign(strerror(errno));
    return false;
  }

  return true;
}
#endif
