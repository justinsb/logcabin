/* Copyright (c) 2013 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "build/Protocol/Raft.pb.h"
#include "Core/Debug.h"
#include "Core/ProtoBuf.h"
#include "Core/StringUtil.h"
#include "Server/Globals.h"
#include "Server/RaftConsensus.h"
#include "Server/StateMachine.h"

namespace LogCabin {
namespace Server {
extern bool stateMachineSuppressThreads;
extern uint32_t stateMachineChildSleepMs;
namespace {

class ServerStateMachineTest : public ::testing::Test {
  public:
    ServerStateMachineTest()
      : globals()
      , consensus()
      , stateMachine()
    {
        RaftConsensusInternal::startThreads = false;
        consensus.reset(new RaftConsensus(globals));
        consensus->serverId = 1;
        consensus->log.reset(new RaftConsensusInternal::Log());
        std::string path = Storage::FilesystemUtil::mkdtemp();
        consensus->storageDirectory =
            Storage::FilesystemUtil::File(open(path.c_str(),
                                               O_RDONLY|O_DIRECTORY),
                                          path);
        RaftConsensusInternal::Log::Entry entry;
        entry.set_term(1);
        entry.set_type(Protocol::Raft::EntryType::CONFIGURATION);
        *entry.mutable_configuration() =
            Core::ProtoBuf::fromString<Protocol::Raft::Configuration>(
                "prev_configuration {"
                "    servers { server_id: 1, address: '127.0.0.1:61023' }"
                "}");
        consensus->init();
        consensus->append(entry);
        consensus->startNewElection();

        stateMachineSuppressThreads = true;
        stateMachine.reset(new StateMachine(consensus));
    }
    ~ServerStateMachineTest() {
        stateMachineSuppressThreads = false;
        stateMachineChildSleepMs = 0;
        Storage::FilesystemUtil::remove(consensus->storageDirectory.path);
    }
    Globals globals;
    std::shared_ptr<RaftConsensus> consensus;
    std::unique_ptr<StateMachine> stateMachine;
};


TEST_F(ServerStateMachineTest, takeSnapshot)
{
    EXPECT_EQ(0U, consensus->lastSnapshotIndex);
    stateMachine->tree.makeDirectory("/foo");
    {
        std::unique_lock<std::mutex> lockGuard(stateMachine->mutex);
        stateMachine->takeSnapshot(1, lockGuard);
    }
    stateMachine->tree.removeDirectory("/foo");
    EXPECT_EQ(1U, consensus->lastSnapshotIndex);
    consensus->discardUnneededEntries();
    consensus->readSnapshot();
    stateMachine->tree.loadSnapshot(consensus->snapshotReader->getStream());
    std::vector<std::string> children;
    stateMachine->tree.listDirectory("/", children);
    EXPECT_EQ((std::vector<std::string>{"foo/"}), children);
}

// This tries to test the use of kill() to stop a snapshotting child and exit
// quickly.
TEST_F(ServerStateMachineTest, applyThreadMain_exiting)
{
    // instruct the child process to sleep for 10s
    stateMachineChildSleepMs = 10000;
    consensus->exit();
    {
        // applyThread won't be able to kill() yet due to mutex
        std::unique_lock<std::mutex> lockGuard(stateMachine->mutex);
        stateMachine->applyThread = std::thread(&StateMachine::applyThreadMain,
                                                stateMachine.get());
        struct timeval startTime;
        EXPECT_EQ(0, gettimeofday(&startTime, NULL));
        stateMachine->takeSnapshot(1, lockGuard);
        struct timeval endTime;
        EXPECT_EQ(0, gettimeofday(&endTime, NULL));
        uint64_t elapsedMillis =
            ((endTime.tv_sec   * 1000 * 1000 + endTime.tv_usec) -
             (startTime.tv_sec * 1000 * 1000 + startTime.tv_usec)) / 1000;
        EXPECT_GT(200U, elapsedMillis) <<
            "This test depends on timing, so failures are likely under "
            "heavy load, valgrind, etc.";
    }
    EXPECT_EQ(0U, consensus->lastSnapshotIndex);
}

} // namespace LogCabin::Server::<anonymous>
} // namespace LogCabin::Server
} // namespace LogCabin

