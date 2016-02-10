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

#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <assert.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "graph.h"
#include "hash_log.h"
#include "test.h"

#ifndef DO_NOT_FORGET_TO_DELETE_THIS
#include <iostream>
#endif

namespace {

const char kTestFilename[] = "HashLogTest-tempfile";

struct TestHashLog : HashLog {
  explicit TestHashLog(FileHasher *hasher) : HashLog(hasher) {}
  // Expose members needed for the tests.
  using HashLog::entries_;
  using HashLog::HashIsClean;
  using HashLog::RecordHash;
};

struct HashLogTest : public testing::Test {
  typedef HashLog::Hash Hash;

  HashLogTest() : log_(&disk_interface_) {}

  void cleanup() {
    unlink(kTestFilename);
  }

  virtual void SetUp() { cleanup(); };
  virtual void TearDown() { cleanup(); };

  VirtualFileSystem disk_interface_;
  TestHashLog log_;
  State state_;
  string err;
};

TEST_F(HashLogTest, NodeInOut) {
  // File does not exist yet.
  Node* node = state_.GetNode("input.txt", 0);

  ASSERT_TRUE(log_.entries_.empty());

  // The file is unknown and does not exist, so it cannot be clean.
  {
    Hash hash = 0;
    ASSERT_FALSE(log_.HashIsClean(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(0u, hash);
    ASSERT_TRUE(log_.entries_.empty());
  }

  // Write the dummy file.
  ASSERT_TRUE(disk_interface_.WriteFile(node->path(), "test"));

  // Check its hash.
  Hash expected_hash;
  ASSERT_EQ(DiskInterface::Okay,
            disk_interface_.HashFile(node->path(), &expected_hash, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(3127628307u, expected_hash);
  ASSERT_EQ(1, disk_interface_.files_read_.size());

  // Update the stat information.
  ASSERT_TRUE(node->Stat(&disk_interface_, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(1, node->mtime());

  // File is still not clean because nothing been recorded yet.
  {
    Hash hash = 0;
    ASSERT_FALSE(log_.HashIsClean(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(0u, hash);
    ASSERT_TRUE(log_.entries_.empty());
    ASSERT_EQ(1, disk_interface_.files_read_.size());
  }

  // To be able to record hashes, log_ has to be opened for writing.
  ASSERT_TRUE(log_.OpenForWrite(kTestFilename, &err));
  ASSERT_TRUE(err.empty());

  // Record the hash.
  {
    Hash hash = 0;
    ASSERT_TRUE(log_.RecordHash(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(expected_hash, hash);
    ASSERT_EQ(1, log_.entries_.size());
    ASSERT_EQ(node->path(), log_.entries_.begin()->first.AsString());
    ASSERT_EQ(node->mtime(), log_.entries_.begin()->second->mtime_);
    ASSERT_EQ(2, disk_interface_.files_read_.size());
  }


  // Next check will yield the node as clean because the mtime is unchanged.
  {
    Hash hash = 0;
    ASSERT_TRUE(log_.HashIsClean(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(expected_hash, hash);
    ASSERT_EQ(1, log_.entries_.size());
    ASSERT_EQ(2, disk_interface_.files_read_.size());
  }

  // Update the file with the same content as before.
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(node->path(), "test"));
  ASSERT_TRUE(node->Stat(&disk_interface_, &err));
  ASSERT_EQ(2, node->mtime());

  // With a changed mtime, the hash will be recomputed.  It's still the same so
  // the node still clean and the updated mtime will be stored.
  {
    Hash hash = 0;
    ASSERT_TRUE(log_.HashIsClean(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(expected_hash, hash);
    ASSERT_EQ(1, log_.entries_.size());
    ASSERT_EQ(node->path(), log_.entries_.begin()->first.AsString());
    ASSERT_EQ(node->mtime(), log_.entries_.begin()->second->mtime_);
    ASSERT_EQ(3, disk_interface_.files_read_.size());
  }

  // Update the file with new content.
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(node->path(), "test_"));
  ASSERT_TRUE(node->Stat(&disk_interface_, &err));
  ASSERT_EQ(3, node->mtime());

  // Recompute the hash.
  ASSERT_EQ(DiskInterface::Okay,
            disk_interface_.HashFile(node->path(), &expected_hash, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(1694588256u, expected_hash);
  ASSERT_EQ(4, disk_interface_.files_read_.size());

  // Now the mtime and the hash are different.  Because the file is already
  // known the new hash and mtime will be recorded.
  {
    Hash hash = 0;
    ASSERT_FALSE(log_.HashIsClean(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(expected_hash, hash);
    ASSERT_EQ(1, log_.entries_.size());
    ASSERT_EQ(node->path(), log_.entries_.begin()->first.AsString());
    ASSERT_EQ(node->mtime(), log_.entries_.begin()->second->mtime_);
    ASSERT_EQ(5, disk_interface_.files_read_.size());
  }

  // Recording the hash again won't do anything because it's already known.
  {
    Hash hash = 0;
    ASSERT_TRUE(log_.RecordHash(node, true, &hash, &err));
    ASSERT_TRUE(err.empty());
    ASSERT_EQ(expected_hash, hash);
    ASSERT_EQ(1, log_.entries_.size());
    ASSERT_EQ(node->path(), log_.entries_.begin()->first.AsString());
    ASSERT_EQ(node->mtime(), log_.entries_.begin()->second->mtime_);
    ASSERT_EQ(5, disk_interface_.files_read_.size());
  }
}

TEST_F(HashLogTest, EdgeInOut) {
  // Create an edge with inputs and outputs.
  Edge edge;
  edge.outputs_.push_back(state_.GetNode("foo.o", 0));
  edge.inputs_.push_back(state_.GetNode("foo.cc", 0));
  edge.inputs_.push_back(state_.GetNode("foo.h", 0));
  edge.inputs_.push_back(state_.GetNode("bar.h", 0));

  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[0]->path(), "void foo() {}"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[1]->path(), "void foo();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[2]->path(), "void bar();"));

  for (size_t i = 0; i < edge.inputs_.size(); ++i) {
    ASSERT_TRUE(edge.inputs_[i]->Stat(&disk_interface_, &err));
  }

  ASSERT_TRUE(edge.outputs_[0]->Stat(&disk_interface_, &err));

  // Open the log.
  ASSERT_TRUE(log_.OpenForWrite(kTestFilename, &err));
  ASSERT_TRUE(err.empty());

  // Hashes are dirty before the first build.
  ASSERT_FALSE(log_.HashesAreClean(edge.outputs_[0], &edge, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(0u, log_.entries_.size());

  // Create the output.
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.outputs_[0]->path(), "_Z3foov"));
  edge.outputs_[0]->ResetState();

  ASSERT_EQ(0u, disk_interface_.files_read_.size());

  // Record hashes.
  ASSERT_TRUE(log_.RecordHashes(&edge, &disk_interface_, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(4u, log_.entries_.size());

  // Recording hashes should have read the three input files.
  ASSERT_EQ(3u, disk_interface_.files_read_.size());
  sort(disk_interface_.files_read_.begin(), disk_interface_.files_read_.end());
  unique(disk_interface_.files_read_.begin(), disk_interface_.files_read_.end());
  ASSERT_EQ(3u, disk_interface_.files_read_.size());

  // The hash log should now contain entries for the inputs and the output.
  ASSERT_EQ(4u, log_.entries_.size());

  for (size_t i = 0; i < disk_interface_.files_read_.size(); ++i) {
    Node *node = state_.GetNode(disk_interface_.files_read_[i], 0);
    HashLog::Entries::iterator it = log_.entries_.find(node->path());
    ASSERT_NE(log_.entries_.end(), it);
    ASSERT_EQ(node->mtime(), it->second->mtime_);
    ASSERT_NE(0u, it->second->input_hash_);
    ASSERT_EQ(0u, it->second->output_hash_);
  }

  {
    Node *node = edge.outputs_[0];
    HashLog::Entries::iterator it = log_.entries_.find(node->path());
    ASSERT_NE(log_.entries_.end(), it);
    ASSERT_EQ(node->mtime(), it->second->mtime_);
    ASSERT_EQ(0u, it->second->input_hash_);
    ASSERT_NE(0u, it->second->output_hash_);
  }

  // Now the hashes should be clean.
  ASSERT_TRUE(log_.HashesAreClean(edge.outputs_[0], &edge, &err));
  ASSERT_TRUE(err.empty());
}

TEST_F(HashLogTest, WriteRead) {
  // Create an edge with inputs and outputs.
  Edge edge;
  edge.outputs_.push_back(state_.GetNode("foo.o", 0));
  edge.inputs_.push_back(state_.GetNode("foo.cc", 0));
  edge.inputs_.push_back(state_.GetNode("foo.h", 0));
  edge.inputs_.push_back(state_.GetNode("bar.h", 0));

  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[0]->path(), "void foo() {}"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[1]->path(), "void foo();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[2]->path(), "void bar();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.outputs_[0]->path(), "_Z3foov"));

  // Open the log.
  ASSERT_TRUE(log_.OpenForWrite(kTestFilename, &err));
  ASSERT_TRUE(err.empty());

  // Record hashes.
  ASSERT_TRUE(log_.RecordHashes(&edge, &disk_interface_, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(4u, log_.entries_.size());

  // Close old log object.
  log_.Close();

  // Open log in new object.
  TestHashLog log2(&disk_interface_);

  ASSERT_TRUE(log2.Load(kTestFilename, &state_, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(4u, log_.entries_.size());

  // Files should still be known to be clean.
  ASSERT_TRUE(log_.HashesAreClean(edge.outputs_[0], &edge, &err));
  ASSERT_TRUE(err.empty());
}

TEST_F(HashLogTest, CheckOnlyFirst) {
  // Create an edge with inputs and outputs.
  Edge edge;
  edge.outputs_.push_back(state_.GetNode("foo.o", 0));
  edge.inputs_.push_back(state_.GetNode("foo.cc", 0));
  edge.inputs_.push_back(state_.GetNode("foo.h", 0));
  edge.inputs_.push_back(state_.GetNode("bar.h", 0));

  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[0]->path(), "void foo() {}"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[1]->path(), "void foo();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[2]->path(), "void bar();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.outputs_[0]->path(), "_Z3foov"));

  // Open the log.
  ASSERT_TRUE(log_.OpenForWrite(kTestFilename, &err));
  ASSERT_TRUE(err.empty());

  // Record hashes.
  ASSERT_TRUE(log_.RecordHashes(&edge, &disk_interface_, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(4u, log_.entries_.size());
  ASSERT_EQ(3u, disk_interface_.files_read_.size());

  // Update the first and second input.
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[0]->path(), "void foo(int) {}"));
  ASSERT_TRUE(edge.inputs_[0]->Stat(&disk_interface_, &err));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[1]->path(), "void foo(int);"));
  ASSERT_TRUE(edge.inputs_[1]->Stat(&disk_interface_, &err));

  disk_interface_.files_read_.clear();

  // Hashes are now dirty.
  ASSERT_FALSE(log_.HashesAreClean(edge.outputs_[0], &edge, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(4u, log_.entries_.size());

  // Only the first input should have been read.
  ASSERT_EQ(1u, disk_interface_.files_read_.size());
  ASSERT_EQ(edge.inputs_[0]->path(), disk_interface_.files_read_[0]);

  // The first inputs's mtime should be updated.
  ASSERT_TRUE(log_.entries_[edge.inputs_[0]->path()]);
  ASSERT_EQ(edge.inputs_[0]->mtime(), log_.entries_[edge.inputs_[0]->path()]->mtime_);

  // The second inputs's mtime should not be updated.
  ASSERT_TRUE(log_.entries_[edge.inputs_[1]->path()]);
  ASSERT_NE(edge.inputs_[1]->mtime(), log_.entries_[edge.inputs_[1]->path()]->mtime_);
}

TEST_F(HashLogTest, TwoEdgesSameInputs) {
  // Create an edge with inputs and outputs.
  Edge edge;
  edge.outputs_.push_back(state_.GetNode("foo.o", 0));
  edge.inputs_.push_back(state_.GetNode("foo.cc", 0));
  edge.inputs_.push_back(state_.GetNode("foo.h", 0));
  edge.inputs_.push_back(state_.GetNode("bar.h", 0));

  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[0]->path(), "void foo() {}"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[1]->path(), "void foo();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.inputs_[2]->path(), "void bar();"));
  disk_interface_.Tick();
  ASSERT_TRUE(disk_interface_.WriteFile(edge.outputs_[0]->path(), "_Z3foov"));

  // Create a second edge with the same inputs but a different output.
  Edge edge2;
  edge2.outputs_.push_back(state_.GetNode("foo-debug.o", 0));
  edge2.inputs_ = edge.inputs_;

  ASSERT_TRUE(disk_interface_.WriteFile(edge2.outputs_[0]->path(), "_Z3foov"));

  // Open the log.
  ASSERT_TRUE(log_.OpenForWrite(kTestFilename, &err));
  ASSERT_TRUE(err.empty());

  // Record hashes for the first edge.
  ASSERT_TRUE(log_.RecordHashes(&edge, &disk_interface_, &err));
  ASSERT_TRUE(err.empty());
  ASSERT_EQ(4u, log_.entries_.size());
  ASSERT_EQ(3u, disk_interface_.files_read_.size());

}

}  // anonymous namespace

// needed tests:
// * repeated inputs (XOR hash combine will fail)
// * error paths: missing output, missing inputs etc.
// * recompacting
// * rerecording the same hash should not increase the log size
// * recording when an output doesn't actually exist
// * when two outputs have the some inputs and only one is rebuilt, the other should still be rebuilt.
// * hash of an output that is also an input
