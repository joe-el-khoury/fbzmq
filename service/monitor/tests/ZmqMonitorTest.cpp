/**
 * Copyright (c) 2014-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <thread>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <fbzmq/service/monitor/ZmqMonitor.h>

using namespace std;
using namespace fbzmq;

using apache::thrift::CompactSerializer;

TEST(ZmqMonitorTest, BasicOperation) {
  SCOPE_EXIT { LOG(INFO) << "ZmqMonitor test/basic operations is done"; };

  LOG(INFO) << "ZmqMonitor test/basic operations starts...";
  Context context;

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  auto monitor = make_shared<ZmqMonitor>(
      std::string{"inproc://monitor-rep"},
      std::string{"inproc://monitor-pub"},
      context);

  auto monitorThread = std::make_unique<std::thread>([monitor]() {
    LOG(INFO) << "ZmqMonitor thread starting";
    monitor->run();
    LOG(INFO) << "ZmqMonitor thread finished";
  });

  // this will be invoked before subscriberThread's d-tor
  SCOPE_EXIT {
    LOG(INFO) << "Stopping the monitor thread";
    monitor->stop();
    monitorThread->join();
  };

  monitor->waitUntilRunning();
  LOG(INFO) << "ZmqMonitor running...";

  // Test with a DEALER socket
  Socket<ZMQ_DEALER, ZMQ_CLIENT> dealer(context);
  dealer.connect(SocketUrl{"inproc://monitor-rep"}).value();
  LOG(INFO) << "dealer sock connected...";

  thrift::MonitorRequest thriftReq;
  thriftReq.cmd = thrift::MonitorCommand::SET_COUNTER_VALUES;
  thrift::Counter counterBar;
  counterBar.value = 1234;
  thriftReq.counterSetParams.counters["bar"] = counterBar;
  thrift::Counter counterFoo;
  counterFoo.value = 5678;
  thriftReq.counterSetParams.counters["foo"] = counterFoo;
  dealer.sendThriftObj(thriftReq, serializer).value();
  LOG(INFO) << "done setting counters...";

  thriftReq.cmd = thrift::MonitorCommand::DUMP_ALL_COUNTER_NAMES;
  dealer.sendThriftObj(thriftReq, serializer).value();
  auto thriftNamesRep =
      dealer.recvThriftObj<thrift::CounterNamesResponse>(serializer).value();
  LOG(INFO) << "got counter names...";

  std::set<std::string> s(
      thriftNamesRep.counterNames.begin(), thriftNamesRep.counterNames.end());
  EXPECT_EQ(std::set<std::string>({"bar", "foo"}), s);

  thriftReq.cmd = thrift::MonitorCommand::GET_COUNTER_VALUES;
  thriftReq.counterGetParams.counterNames = {"bar", "foo"};
  dealer.sendThriftObj(thriftReq, serializer).value();
  auto thriftValuesRep =
      dealer.recvThriftObj<thrift::CounterValuesResponse>(serializer).value();
  LOG(INFO) << "got counter values...";

  EXPECT_EQ(1234, thriftValuesRep.counters["bar"].value);
  EXPECT_EQ(5678, thriftValuesRep.counters["foo"].value);

  // Check the new api of DUMP_ALL_COUNTER_DATA and PUB/SUB as well.
  // First put subscriber in a separate thread to avoid control-flow blocking.
  auto subscriberThread = std::make_unique<std::thread>([&context]() {
    LOG(INFO) << "subscriber thread running";
    auto serializer = apache::thrift::CompactSerializer();

    // Create a subscribe socket.
    Socket<ZMQ_SUB, ZMQ_CLIENT> sub(context);
    sub.connect(SocketUrl{"inproc://monitor-pub"}).value();
    sub.setSockOpt(ZMQ_SUBSCRIBE, "", 0).value();
    LOG(INFO) << "sub socket connected...";

    // Test with the SUB socket.
    // `sub.recv` is blocking call
    {
      auto publication = sub.recvThriftObj<thrift::MonitorPub>(
          serializer).value();
      EXPECT_EQ(thrift::PubType::COUNTER_PUB, publication.pubType);
      auto& updateCounters = publication.counterPub.counters;
      EXPECT_EQ(1, updateCounters.size());
      EXPECT_EQ(9012, updateCounters["foobar"].value);
    }

    {
      auto publication = sub.recvThriftObj<thrift::MonitorPub>(
          serializer).value();
      EXPECT_EQ(thrift::PubType::COUNTER_PUB, publication.pubType);
      auto& updateCounters = publication.counterPub.counters;
      EXPECT_EQ(3, updateCounters.size());
      EXPECT_EQ(1235, updateCounters["bar"].value);
      EXPECT_EQ(5679, updateCounters["foo"].value);
      EXPECT_EQ(1, updateCounters["baz"].value);
    }

    {
      auto publication = sub.recvThriftObj<thrift::MonitorPub>(
          serializer).value();
      EXPECT_EQ(thrift::PubType::EVENT_LOG_PUB, publication.pubType);
      EXPECT_EQ("log_category", publication.eventLogPub.category);
      vector<string> expectedSamples = {"log1", "log2"};
      EXPECT_EQ(expectedSamples, publication.eventLogPub.samples);
    }

    LOG(INFO) << "subscriber thread finishing";
    sub.close();
  });

  // this will be invoked before monitorThread's d-tor
  SCOPE_EXIT {
    LOG(INFO) << "Stopping the subscriber thread";
    subscriberThread->join();
  };

  // Wait a few seconds for subscriber thread to run.
  LOG(INFO) << "main thread pause briefly to let subscriber thread start up...";
  sleep(1);
  LOG(INFO) << "main thread resume...";

  // Add sth extra to the monitor
  thriftReq.cmd = thrift::MonitorCommand::SET_COUNTER_VALUES;
  thrift::Counter counterFoobar;
  counterFoobar.value = 9012;
  thriftReq.counterSetParams.counters.clear();
  thriftReq.counterSetParams.counters["foobar"] = counterFoobar;
  dealer.sendThriftObj(thriftReq, serializer).value();
  LOG(INFO) << "done setting counters again...";

  thriftReq.cmd = thrift::MonitorCommand::DUMP_ALL_COUNTER_DATA;
  dealer.sendThriftObj(thriftReq, serializer).value();
  auto thriftNameValuesRep =
      dealer.recvThriftObj<thrift::CounterValuesResponse>(serializer).value();
  LOG(INFO) << "got all counters dumped from dealer sock...";

  auto& keyValueMap = thriftNameValuesRep.counters;
  EXPECT_EQ(3, keyValueMap.size());
  EXPECT_EQ(1234, keyValueMap["bar"].value);
  EXPECT_EQ(5678, keyValueMap["foo"].value);
  EXPECT_EQ(9012, keyValueMap["foobar"].value);

  // bump some counters
  thriftReq.cmd = thrift::MonitorCommand::BUMP_COUNTER;
  thriftReq.counterBumpParams.counterNames = {"bar", "foo", "baz"};
  dealer.sendThriftObj(thriftReq, serializer).value();
  LOG(INFO) << "done bumping counters ...";

  thriftReq.cmd = thrift::MonitorCommand::DUMP_ALL_COUNTER_DATA;
  dealer.sendThriftObj(thriftReq, serializer).value();
  thriftNameValuesRep =
      dealer.recvThriftObj<thrift::CounterValuesResponse>(serializer).value();
  LOG(INFO) << "got all counters dumped from dealer sock...";

  keyValueMap = thriftNameValuesRep.counters;
  EXPECT_EQ(4, keyValueMap.size());
  // bumped existing counter
  EXPECT_EQ(1235, keyValueMap["bar"].value);
  EXPECT_EQ(5679, keyValueMap["foo"].value);
  // unbumped existing counter
  EXPECT_EQ(9012, keyValueMap["foobar"].value);
  // bumped new counter
  EXPECT_EQ(1, keyValueMap["baz"].value);

  // publish some logs
  thriftReq.cmd = thrift::MonitorCommand::LOG_EVENT;
  thriftReq.eventLog = thrift::EventLog(
      apache::thrift::FRAGILE, "log_category", {"log1", "log2"});
  dealer.sendThriftObj(thriftReq, serializer).value();
  LOG(INFO) << "done publishing logs...";
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
