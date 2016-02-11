// Copyright 2014 Matthias Maennich (matthias@maennich.net)
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

#ifndef NINJA_HASH_LOG_H_
#define NINJA_HASH_LOG_H_

#include <map>
#include <vector>
#include <string>

#include <stdio.h>

#ifdef _WIN32
#include "win32port.h"
#else
#include <stdint.h>
#endif

#include "hash_map.h"
#include "disk_interface.h"

struct Node;
struct Edge;
struct State;

struct HashLog {
  typedef FileHasher::Hash Hash;

  explicit HashLog(FileHasher *hasher);
  ~HashLog();

  bool Load(const string& path, State* state, string* err);

  /// Check whether an edge's input and output hashes match previously
  /// recorded values.  The stat information on the inputs and outputs
  /// must be current for this to give the correct result.
  bool HashesAreClean(Node *output, Edge* edge, string* err);

  bool OpenForWrite(const string &path, string* err);
  void Close();

  /// Persist hashes (inputs and outputs) for a finished edge.
  bool RecordHashes(Edge* edge, DiskInterface* disk_interface, string* err);

  /// Recompact the hash log to reduce it to minimum size
  bool Recompact(const string &path, string* err);

#if 0
  struct LogEntry {
    /// Unique id for each node so paths do not need to be stored for each
    /// record.
    unsigned id_;


    /// When the hash.
    struct Input {
      unsigned id_;
      Hash value_;

      Input() : id_(0), value_(0) {}
    };

    unsigned input_count_;
    Input *inputs_;

    LogEntry() : id_(0), mtime_(0), input_hash_(0), output_hash_(0) {}
  };
#endif

  /// Path to node id mappings.
  typedef ExternalStringHashMap<unsigned>::Type Ids;

  /// Hash record for a node.
  struct HashRecord {
    /// The timestamp of the file when the hash was computed.  Hashes are only
    /// recomputed if the timestamp is different.
    TimeStamp mtime_;
    /// The size of the file when the hash was computed.  When comparing hashes
    /// the file size is compared first.
    unsigned size;
    /// The hash value.
    Hash value_;

    HashRecord() : mtime_(0), size(0), value_(0) {}
  };

  struct IdHashRecord : HashRecord {
    /// Id of the node this hash is for.
    unsigned id_;

    IdHashRecord() : id_(0) {}
  };

  /// Record for an output of an edge.
  struct OutputRecord {
    /// Records of all inputs sorted by the id.
    typedef vector<IdHashRecord> Inputs;
    Inputs inputs_;
  };

 protected:
#if 0
  bool WriteEntry(Node *node, LogEntry *entry, string *err);
#endif

  OutputRecord *GetOutputRecord(Node *node) const;
  IdHashRecord *GetInputRecord(Node *node, OutputRecord *output) const;

  bool HashIsClean(Node *node, IdHashRecord *input, string* err);

  // Hash GetHash(Node *node, string* err);

#if 0
  bool HashIsClean(Node *node, bool is_input, Hash *acc, string *err);
#endif

  bool RecordHash(Node *node, bool is_input, Hash *acc, string *err);

  unsigned next_id_;

  FILE* file_;

  FileHasher *hasher_;

  /// Map path names to ids.
  Ids ids_;
  /// Output records indexed by id.
  vector<OutputRecord*> outputs_;
  /// Last computed hash of an input by id.
  vector<HashRecord*> hashes_;

  bool needs_recompaction_;
};

#endif //NINJA_HASH_LOG_H_
