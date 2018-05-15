/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>
#include <regex>
#include <stdexcept>
#include <syslog.h>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/common/BuildInfo.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/health-checker/HealthChecker.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/platform/NetlinkSystemHandler.h>
#include <openr/platform/PlatformPublisher.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/prefix-manager/PrefixManagerClient.h>
#include <openr/spark/Spark.h>
#include <openr/watchdog/Watchdog.h>

DEFINE_int32(
    kvstore_pub_port,
    openr::Constants::kKvStorePubPort,
    "KvStore publisher port for emitting realtime key-value deltas");
DEFINE_int32(kvstore_rep_port,
    openr::Constants::kKvStoreRepPort, "The port KvStore replier listens on");
DEFINE_int32(
    decision_pub_port,
    openr::Constants::kDecisionPubPort,
    "Decision publisher port for emitting realtime route-db updates");
DEFINE_int32(decision_rep_port,
    openr::Constants::kDecisionRepPort,
    "The port Decision replier listens on");
DEFINE_int32(
    link_monitor_pub_port,
    openr::Constants::kLinkMonitorPubPort,
    "The port link monitor publishes on");
DEFINE_int32(
    link_monitor_cmd_port,
    openr::Constants::kLinkMonitorCmdPort,
    "The port link monitor listens for commands on ");
DEFINE_int32(monitor_pub_port,
    openr::Constants::kMonitorPubPort, "The port monitor publishes on");
DEFINE_int32(monitor_rep_port,
    openr::Constants::kMonitorRepPort, "The port monitor replies on");
DEFINE_int32(fib_rep_port,
    openr::Constants::kFibRepPort, "The port fib replier listens on");
DEFINE_int32(
    health_checker_port,
    openr::Constants::kHealthCheckerPort,
    "The port health checker sends and recvs udp pings on");
DEFINE_int32(
    prefix_manager_cmd_port,
    openr::Constants::kPrefixManagerCmdPort,
    "The port prefix manager receives commands on");
DEFINE_int32(
    health_checker_rep_port,
    openr::Constants::kHealthCheckerRepPort,
    "The port Health Checker replier listens on");
DEFINE_int32(
    system_agent_port,
    openr::Constants::kSystemAgentPort,
    "Switch agent thrift service port for Platform programming.");
DEFINE_int32(
    fib_handler_port,
    openr::Constants::kFibAgentPort, // NOTE 100 is on purpose
    "Switch agent thrift service port for FIB programming.");
DEFINE_int32(
    spark_mcast_port,
    openr::Constants::kSparkMcastPort,
    "Spark UDP multicast port for sending spark-hello messages.");
DEFINE_string(
    platform_pub_url,
    "ipc:///tmp/platform-pub-url",
    "Publisher URL for interface/address notifications");
DEFINE_string(
    domain,
    "terragraph",
    "Domain name associated with this OpenR. No adjacencies will be formed "
    "to OpenR of other domains.");
DEFINE_string(
    chdir, "/tmp", "Change current directory to this after loading config");
DEFINE_string(listen_addr, "*", "The IP address to bind to");
DEFINE_string(
    config_store_filepath,
    "/tmp/aq_persistent_config_store.bin",
    "File name where to persist OpenR's internal state across restarts");
DEFINE_bool(
    assume_drained,
    false,
    "If set, will assume node is drained if no drain state is found in the "
    "persistent store");
DEFINE_string(
    node_name,
    "node1",
    "The name of current node (also serves as originator id");
DEFINE_bool(
    dryrun, true, "Run the process in dryrun mode. No FIB programming!");
DEFINE_string(loopback_iface, "lo", "The iface to configure with the prefix");
DEFINE_string(
    prefixes,
    "",
    "The prefix and loopback IP separated by comma for this node");
DEFINE_string(
    seed_prefix,
    "",
    "The seed prefix all subprefixes are to be allocated from. If empty, "
    "it will be injected later together with allocated prefix length");
DEFINE_bool(enable_prefix_alloc, false, "Enable automatic prefix allocation");
DEFINE_int32(alloc_prefix_len, 128, "Allocated prefix length");
DEFINE_bool(static_prefix_alloc, false, "Perform static prefix allocation");
DEFINE_bool(
    set_loopback_address,
    false,
    "Set the IP addresses from supplied prefix param to loopback (/128)");
DEFINE_bool(
    override_loopback_addr,
    false,
    "If enabled then all global addresses assigned on loopback will be flushed "
    "whenever OpenR elects new prefix for node. Only effective when prefix "
    "allocator is turned on and set_loopback_address is also turned on");
DEFINE_string(
    ifname_prefix,
    "terra,nic1,nic2",
    "A comma separated list of strings. Linux interface names with a prefix "
    "matching at least one will be used for neighbor discovery, provided the "
    "interface is not excluded by the flag iface_regex_exclude");
DEFINE_string(
    iface_regex_include,
    "",
    "A comma separated list of extended POSIX regular expressions. Linux "
    "interface names containing a match (case insensitive) to at least one of "
    "these and not excluded by the flag iface_regex_exclude will be used for "
    "neighbor discovery");
DEFINE_string(
    iface_regex_exclude,
    "",
    "A comma separated list of extended POSIX regular expressions. Linux "
    "interface names containing a match (case insensitive) to at least one of "
    "these will not be used for neighbor discovery");
DEFINE_string(
    redistribute_ifaces,
    "",
    "The interface names or regex who's prefixes we want to advertise");
DEFINE_bool(enable_auth, false, "Enable known keys authentication in Spark");
DEFINE_string(known_keys_file_path, "/tmp/known_keys.json", "Known keys file");
DEFINE_string(
    cert_file_path,
    "/tmp/cert_node_1.json",
    "my certificate file containing private & public key pair");
DEFINE_bool(enable_encryption, false, "Encrypt traffic between AQ instances");
DEFINE_bool(
    enable_rtt_metric,
    true,
    "Use dynamically learned RTT for interface metric values.");
DEFINE_bool(
    enable_full_mesh_reduction,
    false,
    "mesh reduction on full mesh topology to avoid duplicate flooding");
DEFINE_bool(
    enable_v4,
    false,
    "Enable v4 in OpenR for exchanging and programming v4 routes. Works only"
    "when Switch FIB Agent is used for FIB programming. No NSS/Linux.");
DEFINE_int32(
    spark_hold_time_s,
    18,
    "How long (in seconds) to keep neighbor adjacency without receiving any"
    "hello packets.");
DEFINE_int32(
    spark_keepalive_time_s,
    2,
    "Keep-alive message interval (in seconds) for spark hello message"
    "exchanges. At most 2 hello message exchanges are required for graceful"
    "restart.");
DEFINE_int32(
    spark_fastinit_keepalive_time_ms,
    100,
    "Fast initial keep alive time (in milliseconds)");
DEFINE_int32(
    health_checker_ping_interval_s,
    10,
    "Time interval (in seconds) to send health check pings to other nodes in"
    "the network.");
DEFINE_bool(
    enable_health_checker,
    false,
    "If set, will send pings to other nodes in network at interval specified by"
    "health_checker_ping_interval flag");
DEFINE_bool(enable_fib_sync, false, "Enable periodic syncFib to FibAgent");
DEFINE_int32(
    health_check_option,
    static_cast<uint32_t>(
        openr::thrift::HealthCheckOption::PingNeighborOfNeighbor),
    "Health check scenarios, default set as ping neighbor of neighbor");
DEFINE_int32(
    health_check_pct, 50, "Health check pct % of nodes in entire topology");
DEFINE_bool(
    enable_spark_signature,
    false,
    "Enable the spark packet signature. Disabling It can save you some "
    "CPU cycles");
DEFINE_bool(
    enable_netlink_fib_handler,
    false,
    "If set, netlink fib handler will be started for route programming.");
DEFINE_bool(
    enable_netlink_system_handler,
    true,
    "If set, netlink system handler will be started");
DEFINE_int32(
    ip_tos,
    openr::Constants::kIpTos,
    "Mark control plane traffic with specified IP-TOS value. Set this to 0 "
    "if you don't want to mark packets.");
DEFINE_int32(
    zmq_context_threads,
    1,
    "Number of ZMQ Context thread to use for IO processing.");
DEFINE_int32(
    link_flap_initial_backoff_ms,
    1000,
    "initial backoff to dampen link flaps (in seconds)");
DEFINE_int32(
    link_flap_max_backoff_ms,
    60000,
    "max backoff to dampen link flaps (in seconds)");
DEFINE_bool(
    enable_perf_measurement,
    true,
    "Enable performance measurement in network.");
DEFINE_int32(
    decision_debounce_min_ms,
    10,
    "Fast reaction time to update decision spf upon receiving adj db update "
    "(in milliseconds)");
DEFINE_int32(
    decision_debounce_max_ms,
    250,
    "Decision debounce time to update spf in frequent adj db update "
    "(in milliseconds)");
DEFINE_bool(
    enable_watchdog,
    true,
    "Enable watchdog thread to periodically check aliveness counters from each "
    "openr thread, if unhealthy thread is detected, force crash openr");
DEFINE_int32(watchdog_interval_s, 20, "Watchdog thread healthcheck interval");
DEFINE_int32(watchdog_threshold_s, 300, "Watchdog thread aliveness threshold");
DEFINE_bool(
    advertise_interface_db,
    false,
    "Flag to optionally advertise interface-DB information in KvStore.");
DEFINE_bool(
    enable_segment_routing,
    false,
    "Flag to disable/enable segment routing");
DEFINE_bool(
    set_leaf_node,
    false,
    "Flag to enable/disable node as a leaf node");
DEFINE_string(
    key_prefix_filters,
    "",
    "Only keys matching any of the prefixes in the list "
    "will be added to kvstore");
DEFINE_string(
    key_originator_id_filters,
    "",
    "Only keys with originator ID matching any of the originator ID will "
    "be added to kvstore.");

using namespace fbzmq;
using namespace openr;

using namespace folly::gen;

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;
using apache::thrift::concurrency::ThreadManager;

// Disable background jemalloc background thread => new jemalloc-5 feature
const char* malloc_conf = "background_thread:false";

namespace {
//
// Local constants
//

// the URL for the spark server
const SparkReportUrl kSparkReportUrl{"inproc://spark_server_report"};

// the URL for the spark server
const SparkCmdUrl kSparkCmdUrl{"inproc://spark_server_cmd"};

// the URL for Decision module
const DecisionPubUrl kDecisionPubUrl{"inproc://decision_server_pub"};

// the URL Prefix for the ConfigStore module
const PersistentStoreUrl kConfigStoreUrl{"ipc:///tmp/openr_config_store_cmd"};

const PrefixManagerLocalCmdUrl kPrefixManagerLocalCmdUrl{
    "inproc://prefix_manager_cmd_local"};

const fbzmq::SocketUrl kForceCrashServerUrl{"ipc:///tmp/force_crash_server"};

} // namespace

int
main(int argc, char** argv) {
  // Initialize syslog
  // We log all messages upto INFO level.
  // LOG_CONS => Log to console on error
  // LOG_PID => Log PID along with each message
  // LOG_NODELAY => Connect immediately
  setlogmask(LOG_UPTO(LOG_INFO));
  openlog("openr", LOG_CONS | LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_LOCAL4);
  syslog(LOG_NOTICE, "Starting OpenR daemon....hi from public openr and changes.");

  // Log build information
  BuildInfo::log(std::cout);

  // Initialize all params
  folly::init(&argc, &argv);

  // start signal handler before any thread
  ZmqEventLoop mainEventLoop;
  StopEventLoopSignalHandler handler(&mainEventLoop);
  handler.registerSignalHandler(SIGINT);
  handler.registerSignalHandler(SIGQUIT);
  handler.registerSignalHandler(SIGTERM);

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Sanity checks on Segment Routing labels
  const int32_t maxLabel = Constants::kMaxSrLabel;
  CHECK(Constants::kSrGlobalRange.first > 0);
  CHECK(Constants::kSrGlobalRange.second < maxLabel);
  CHECK(Constants::kSrLocalRange.first > 0);
  CHECK(Constants::kSrLocalRange.second < maxLabel);
  CHECK(Constants::kSrGlobalRange.first < Constants::kSrGlobalRange.second);
  CHECK(Constants::kSrLocalRange.first < Constants::kSrLocalRange.second);

  // Local and Global range must be exclusive of each other
  CHECK(
      (Constants::kSrGlobalRange.second < Constants::kSrLocalRange.first) ||
      (Constants::kSrGlobalRange.first > Constants::kSrLocalRange.second))
      << "Overlapping global/local segment routing label space.";

  // Prepare IP-TOS value from flag and do sanity checks
  folly::Optional<int> maybeIpTos{0};
  if (FLAGS_ip_tos != 0) {
    CHECK_LE(0, FLAGS_ip_tos) << "ip_tos must be greater than 0";
    CHECK_GE(256, FLAGS_ip_tos) << "ip_tos must be less than 256";
    maybeIpTos = FLAGS_ip_tos;
  }

  std::vector<std::thread> allThreads{};

  // change directory after the config has been loaded
  ::chdir(FLAGS_chdir.c_str());

  // Set up the zmq context for this process.
  Context context;

  // Register force crash handler
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> forceCrashServer{
      context, folly::none, folly::none, fbzmq::NonblockingFlag{true}};
  auto ret = forceCrashServer.bind(kForceCrashServerUrl);
  if (ret.hasError()) {
    LOG(ERROR) << "Failed to bind on " << std::string(kForceCrashServerUrl);
    return 1;
  }
  mainEventLoop.addSocket(
      fbzmq::RawZmqSocketPtr{*forceCrashServer}, ZMQ_POLLIN,
      [&](int) noexcept {
        auto msg = forceCrashServer.recvOne();
        if (msg.hasError()) {
          LOG(ERROR) << "Failed receiving message on forceCrashServer.";
        }
        LOG(FATAL) << "Triggering forceful crash. "
                   << msg->read<std::string>().value();
      });

  // Hack to assign different thread name to ZMQ threads for brevity. Bind
  // starts zmq ctx and reaper threads
  folly::setThreadName("zmq_ctx_reaper");
  {
    Socket<ZMQ_REP, ZMQ_SERVER> tmpSock(context);
    tmpSock.bind(SocketUrl{"ipc://*"}).value();
  }
  folly::setThreadName("openr");

  // Watchdog thread to monitor thread aliveness
  std::unique_ptr<Watchdog> watchdog{nullptr};
  if (FLAGS_enable_watchdog) {
    watchdog = std::make_unique<Watchdog>(
      FLAGS_node_name,
      std::chrono::seconds(FLAGS_watchdog_interval_s),
      std::chrono::seconds(FLAGS_watchdog_threshold_s));

    // Spawn a watchdog thread
    allThreads.emplace_back(std::thread([&watchdog]() noexcept {
      LOG(INFO) << "Starting Watchdog thread ...hi 12324";
      folly::setThreadName("Watchdog");
      watchdog->run();
      LOG(INFO) << "Watchdog thread got stopped.";
    }));
    watchdog->waitUntilRunning();
  }

  // Create ThreadManager for thrift services
  std::shared_ptr<ThreadManager> thriftThreadMgr;
  if (FLAGS_enable_netlink_fib_handler or FLAGS_enable_netlink_system_handler) {
    thriftThreadMgr = ThreadManager::newPriorityQueueThreadManager(
        2 /* num of threads */,
        false /* task stats */);
    thriftThreadMgr->setNamePrefix("ThriftCpuPool");
    thriftThreadMgr->start();
  }

  // Start NetlinkFibHandler if specified
  std::unique_ptr<apache::thrift::ThriftServer> netlinkFibServer;
  if (FLAGS_enable_netlink_fib_handler) {
    CHECK(thriftThreadMgr);
    netlinkFibServer = std::make_unique<apache::thrift::ThriftServer>(
        "disabled" /* sasl policy */, false /* insecure-loopback */);
    netlinkFibServer->setThreadManager(thriftThreadMgr);
    netlinkFibServer->setNumIOWorkerThreads(1);
    netlinkFibServer->setCpp2WorkerThreadName("FibTWorker");
    netlinkFibServer->setPort(FLAGS_fib_handler_port);

    auto fibThriftThread = std::thread([&netlinkFibServer, &mainEventLoop]() {
      folly::setThreadName("FibService");
      auto fibHandler = std::make_shared<NetlinkFibHandler>(&mainEventLoop);
      netlinkFibServer->setInterface(fibHandler);

      LOG(INFO) << "Starting NetlinkFib server...";
      netlinkFibServer->serve();
      LOG(INFO) << "NetlinkFib server got stopped.";
    });
    allThreads.emplace_back(std::move(fibThriftThread));
  }

  // Start NetlinkSystemHandler if specified
  std::unique_ptr<apache::thrift::ThriftServer> netlinkSystemServer;
  if (FLAGS_enable_netlink_system_handler) {
    CHECK(thriftThreadMgr);
    netlinkSystemServer = std::make_unique<apache::thrift::ThriftServer>(
        "disabled" /* sasl policy */, false /* insecure-loopback */);
    netlinkSystemServer->setThreadManager(thriftThreadMgr);
    netlinkSystemServer->setNumIOWorkerThreads(1);
    netlinkSystemServer->setCpp2WorkerThreadName("SystemTWorker");
    netlinkSystemServer->setPort(FLAGS_system_agent_port);

    auto systemThriftThread =
        std::thread([&netlinkSystemServer, &context, &mainEventLoop]() {
          folly::setThreadName("SystemService");
          auto systemHandler = std::make_unique<NetlinkSystemHandler>(
              context,
              PlatformPublisherUrl{FLAGS_platform_pub_url},
              &mainEventLoop);
          netlinkSystemServer->setInterface(std::move(systemHandler));

          LOG(INFO) << "Starting NetlinkSystem server...";
          netlinkSystemServer->serve();
          LOG(INFO) << "NetlinkSystem server got stopped.";
        });
    allThreads.emplace_back(std::move(systemThriftThread));
  }

  folly::Optional<KeyPair> keyPair;
  if (FLAGS_enable_auth) {
    auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
    keyPair = loadKeyPairFromFile(FLAGS_cert_file_path, jsonSerializer);
  } else {
    keyPair = util::genKeyPair();
  }

  const KvStoreLocalPubUrl kvStoreLocalPubUrl{"inproc://kvstore_pub_local"};
  const KvStoreLocalCmdUrl kvStoreLocalCmdUrl{"inproc://kvstore_cmd_local"};
  const MonitorSubmitUrl monitorSubmitUrl{
      folly::sformat("tcp://[::1]:{}", FLAGS_monitor_rep_port)};

  // Start config-store URL
  PersistentStore configStore(
      FLAGS_config_store_filepath, kConfigStoreUrl, context);
  std::thread configStoreThread([&configStore]() noexcept {
    LOG(INFO) << "Starting ConfigStore thread...";
    folly::setThreadName("ConfigStore");
    configStore.run();
    LOG(INFO) << "ConfigStore thread got stopped.";
  });
  configStore.waitUntilRunning();
  allThreads.emplace_back(std::move(configStoreThread));

  // Start monitor Module
  // for each log message it receives, we want to add the openr domain
  fbzmq::LogSample sampleToMerge;
  sampleToMerge.addString("domain", FLAGS_domain);
  ZmqMonitor monitor(
      MonitorSubmitUrl{folly::sformat(
          "tcp://{}:{}", FLAGS_listen_addr, FLAGS_monitor_rep_port)},
      MonitorPubUrl{folly::sformat(
          "tcp://{}:{}", FLAGS_listen_addr, FLAGS_monitor_pub_port)},
      context,
      sampleToMerge);
  std::thread monitorThread([&monitor]() noexcept {
    LOG(INFO) << "Starting ZmqMonitor thread...";
    folly::setThreadName("ZmqMonitor");
    monitor.run();
    LOG(INFO) << "ZmqMonitor thread got stopped.";
  });
  monitor.waitUntilRunning();
  allThreads.emplace_back(std::move(monitorThread));

  folly::Optional<KvStoreFilters> kvFilters = folly::none;
  // Add key prefixes to allow if set as leaf node
  if (FLAGS_set_leaf_node) {
    std::vector<std::string> keyPrefixList;
    folly::split(",", FLAGS_key_prefix_filters, keyPrefixList, true);

    // save nodeIds in the set
    std::set<std::string> originatorIds{};
    folly::splitTo<std::string>(
      ",",
      FLAGS_key_originator_id_filters,
      std::inserter(originatorIds, originatorIds.begin()),
      true);

    keyPrefixList.push_back(Constants::kPrefixAllocMarker.toString());
    keyPrefixList.push_back(Constants::kNodeLabelRangePrefix.toString());
    originatorIds.insert(FLAGS_node_name);
    kvFilters = KvStoreFilters(keyPrefixList, originatorIds);
  }

  // Start KVStore
  KvStore store(
      context,
      FLAGS_node_name,
      kvStoreLocalPubUrl,
      KvStoreGlobalPubUrl{folly::sformat(
          "tcp://{}:{}", FLAGS_listen_addr, FLAGS_kvstore_pub_port)},
      kvStoreLocalCmdUrl,
      KvStoreGlobalCmdUrl{folly::sformat(
          "tcp://{}:{}", FLAGS_listen_addr, FLAGS_kvstore_rep_port)},
      monitorSubmitUrl,
      maybeIpTos,
      FLAGS_enable_encryption ? keyPair : folly::none,
      Constants::kStoreSyncInterval,
      Constants::kMonitorSubmitInterval,
      std::unordered_map<std::string, openr::thrift::PeerSpec>{},
      std::move(kvFilters));
  std::thread kvStoreThread([&store]() noexcept {
    LOG(INFO) << "Starting KvStore thread...";
    folly::setThreadName("KvStore");
    store.run();
    LOG(INFO) << "KvStore thread got stopped.";
  });
  store.waitUntilRunning();
  allThreads.emplace_back(std::move(kvStoreThread));
  watchdog->addEvl(&store, "KvStore");

  // start prefix manager
  PrefixManager prefixManager(
      FLAGS_node_name,
      PrefixManagerGlobalCmdUrl{
          folly::sformat("tcp://*:{}", FLAGS_prefix_manager_cmd_port)},
      kPrefixManagerLocalCmdUrl,
      kConfigStoreUrl,
      kvStoreLocalCmdUrl,
      kvStoreLocalPubUrl,
      FLAGS_enable_encryption ? keyPair : folly::none,
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      FLAGS_enable_perf_measurement,
      monitorSubmitUrl,
      context);

  allThreads.emplace_back(std::thread([&prefixManager]() noexcept {
    LOG(INFO) << "Starting the PrefixManager thread...";
    folly::setThreadName("PrefixManager");
    prefixManager.run();
    LOG(INFO) << "PrefixManager thread got stopped.";
  }));
  watchdog->addEvl(&prefixManager, "PrefixManager");
  prefixManager.waitUntilRunning();

  // Prefix Allocator to automatically allocate prefixes for nodes
  std::unique_ptr<PrefixAllocator> prefixAllocator;
  if (FLAGS_enable_prefix_alloc) {
    // start prefix allocator
    PrefixAllocatorMode allocMode;
    if (FLAGS_static_prefix_alloc) {
      allocMode = PrefixAllocatorModeStatic();
    } else if (!FLAGS_seed_prefix.empty()) {
      allocMode = std::make_pair(
          folly::IPAddress::createNetwork(FLAGS_seed_prefix),
          static_cast<uint8_t>(FLAGS_alloc_prefix_len));
    } else {
      allocMode = PrefixAllocatorModeSeeded();
    }
    prefixAllocator = std::make_unique<PrefixAllocator>(
        FLAGS_node_name,
        kvStoreLocalCmdUrl,
        kvStoreLocalPubUrl,
        kPrefixManagerLocalCmdUrl,
        monitorSubmitUrl,
        AllocPrefixMarker{Constants::kPrefixAllocMarker.toString()},
        allocMode,
        FLAGS_set_loopback_address,
        FLAGS_override_loopback_addr,
        FLAGS_loopback_iface,
        Constants::kPrefixAllocatorSyncInterval,
        kConfigStoreUrl,
        context);
    // Spawn a PrefixAllocator thread
    allThreads.emplace_back(std::thread([&prefixAllocator]() noexcept {
      LOG(INFO) << "Starting PrefixAllocator thread ...";
      folly::setThreadName("PrefixAllocator");
      prefixAllocator->run();
      LOG(INFO) << "PrefixAllocator thread got stopped.";
    }));
    watchdog->addEvl(prefixAllocator.get(), "PrefixAllocator");
    prefixAllocator->waitUntilRunning();
  }

  //
  // Start the spark service. For now, use random key-pair
  //
  KnownKeysStore knownKeysStore(FLAGS_known_keys_file_path);

  Spark spark(
      FLAGS_domain, // My domain
      FLAGS_node_name, // myNodeName
      static_cast<uint16_t>(FLAGS_spark_mcast_port),
      std::chrono::seconds(FLAGS_spark_hold_time_s),
      std::chrono::seconds(FLAGS_spark_keepalive_time_s),
      std::chrono::milliseconds(FLAGS_spark_fastinit_keepalive_time_ms),
      maybeIpTos,
      keyPair.value(),
      FLAGS_enable_auth ? &knownKeysStore : nullptr,
      FLAGS_enable_v4,
      FLAGS_enable_spark_signature,
      kSparkReportUrl,
      kSparkCmdUrl,
      monitorSubmitUrl,
      KvStorePubPort{static_cast<uint16_t>(FLAGS_kvstore_pub_port)},
      KvStoreCmdPort{static_cast<uint16_t>(FLAGS_kvstore_rep_port)},
      std::make_pair(Constants::kOpenrVersion,
                     Constants::kOpenrSupportedVersion),
      context);

  std::thread sparkThread([&spark]() noexcept {
    LOG(INFO) << "Starting the spark thread...";
    folly::setThreadName("Spark");
    spark.run();
    LOG(INFO) << "Spark thread got stopped.";
  });
  spark.waitUntilRunning();
  allThreads.emplace_back(std::move(sparkThread));
  watchdog->addEvl(&spark, "Spark");

  // Static list of prefixes to announce into the network as long as OpenR is
  // running.
  std::vector<openr::thrift::IpPrefix> networks;
  try {
    std::vector<std::string> prefixes;
    folly::split(",", FLAGS_prefixes, prefixes, true /* ignore empty */);
    for (auto const& prefix : prefixes) {
      // Perform some sanity checks before announcing the list of prefixes
      auto network = folly::IPAddress::createNetwork(prefix);
      if (network.first.isLoopback()) {
        LOG(FATAL) << "Default loopback addresses can't be announced "
                   << prefix;
      }
      if (network.first.isLinkLocal()) {
        LOG(FATAL) << "Link local addresses can't be announced " << prefix;
      }
      if (network.first.isMulticast()) {
        LOG(FATAL) << "Multicast addresses can't be annouced " << prefix;
      }
      networks.emplace_back(toIpPrefix(network));
    }
  } catch (std::exception const& err) {
    LOG(ERROR) << "Invalid Prefix string specified. Expeted comma separated "
               << "list of IP/CIDR format, got '" << FLAGS_prefixes << "'";
    return -1;
  }


  //
  // Construct the regular expressions to match interface names against
  //

  std::vector<std::string> regexIncludeStrings;
  folly::split(",", FLAGS_iface_regex_include, regexIncludeStrings, true);
  std::vector<std::regex> includeRegexList;
  auto const regexOpts = std::regex_constants::extended |
      std::regex_constants::icase | std::regex_constants::optimize;

  for (auto& regexStr : regexIncludeStrings) {
    includeRegexList.emplace_back(regexStr, regexOpts);
  }
  // add prefixes
  std::vector<std::string> ifNamePrefixes;
  folly::split(",", FLAGS_ifname_prefix, ifNamePrefixes, true);
  for (auto& prefix : ifNamePrefixes) {
    includeRegexList.emplace_back(prefix + ".*", regexOpts);
  }

  std::vector<std::string> regexExcludeStrings;
  folly::split(",", FLAGS_iface_regex_exclude, regexExcludeStrings, true);
  std::vector<std::regex> excludeRegexList;
  for (auto& regexStr : regexExcludeStrings) {
    excludeRegexList.emplace_back(regexStr, regexOpts);
  }

  std::vector<std::string> redistStringList;
  folly::split(",", FLAGS_redistribute_ifaces, redistStringList, true);
  std::vector<std::regex> redistRegexList;
  for (auto const& regexStr : redistStringList) {
    redistRegexList.emplace_back(regexStr, regexOpts);
  }

  // Create link monitor instance.
  LinkMonitor linkMonitor(
      context,
      FLAGS_node_name,
      FLAGS_system_agent_port,
      KvStoreLocalCmdUrl{kvStoreLocalCmdUrl},
      KvStoreLocalPubUrl{kvStoreLocalPubUrl},
      includeRegexList,
      excludeRegexList,
      redistRegexList,
      networks,
      FLAGS_enable_rtt_metric,
      FLAGS_enable_full_mesh_reduction,
      FLAGS_enable_perf_measurement,
      FLAGS_enable_v4,
      FLAGS_advertise_interface_db,
      FLAGS_enable_segment_routing,
      AdjacencyDbMarker{Constants::kAdjDbMarker.toString()},
      InterfaceDbMarker{Constants::kInterfaceDbMarker.toString()},
      SparkCmdUrl{kSparkCmdUrl},
      SparkReportUrl{kSparkReportUrl},
      monitorSubmitUrl,
      kConfigStoreUrl,
      FLAGS_assume_drained,
      kPrefixManagerLocalCmdUrl,
      PlatformPublisherUrl{FLAGS_platform_pub_url},
      LinkMonitorGlobalPubUrl{
          folly::sformat("tcp://*:{}", FLAGS_link_monitor_pub_port)},
      LinkMonitorGlobalCmdUrl{
          folly::sformat("tcp://*:{}", FLAGS_link_monitor_cmd_port)},
      std::chrono::seconds(2 * FLAGS_spark_keepalive_time_s),
      std::chrono::milliseconds(FLAGS_link_flap_initial_backoff_ms),
      std::chrono::milliseconds(FLAGS_link_flap_max_backoff_ms));

  // start link monitor thread
  std::thread linkMonitorThread([&linkMonitor]() noexcept {
    LOG(INFO) << "Starting LinkMonitor thread...";
    folly::setThreadName("LinkMonitor");
    linkMonitor.run();
    LOG(INFO) << "LinkMonitor thread got stopped.";
  });
  linkMonitor.waitUntilRunning();
  allThreads.emplace_back(std::move(linkMonitorThread));
  watchdog->addEvl(&linkMonitor, "LinkMonitor");

  // Wait for the above two threads to start and run before running
  // SPF in Decision module.  This is to make sure the Decision module
  // receives itself as one of the nodes before running the spf.

  // Start Decision Module
  Decision decision(
      FLAGS_node_name,
      FLAGS_enable_v4,
      AdjacencyDbMarker{Constants::kAdjDbMarker.toString()},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      std::chrono::milliseconds(FLAGS_decision_debounce_min_ms),
      std::chrono::milliseconds(FLAGS_decision_debounce_max_ms),
      kvStoreLocalCmdUrl,
      kvStoreLocalPubUrl,
      DecisionCmdUrl{folly::sformat(
          "tcp://{}:{}", FLAGS_listen_addr, FLAGS_decision_rep_port)},
      kDecisionPubUrl,
      monitorSubmitUrl,
      context);
  std::thread decisionThread([&decision]() noexcept {
    LOG(INFO) << "Starting Decision thread...";
    folly::setThreadName("Decision");
    decision.run();
    LOG(INFO) << "Decision thread got stopped.";
  });
  decision.waitUntilRunning();
  allThreads.emplace_back(std::move(decisionThread));
  watchdog->addEvl(&decision, "Decision");

  // Define and start Fib Module
  Fib fib(
      FLAGS_node_name,
      FLAGS_fib_handler_port,
      FLAGS_dryrun,
      FLAGS_enable_fib_sync,
      std::chrono::seconds(3 * FLAGS_spark_keepalive_time_s),
      kDecisionPubUrl,
      FibCmdUrl{
          folly::sformat("tcp://{}:{}", FLAGS_listen_addr, FLAGS_fib_rep_port)},
      LinkMonitorGlobalPubUrl{
          folly::sformat("tcp://[::1]:{}", FLAGS_link_monitor_pub_port)},
      monitorSubmitUrl,
      context);

  // Spawn a FIB thread
  allThreads.emplace_back(std::thread([&fib]() noexcept {
    LOG(INFO) << "Starting FIB thread ...";
    folly::setThreadName("Fib");
    fib.run();
    LOG(INFO) << "FIB thread got stopped.";
  }));
  fib.waitUntilRunning();
  watchdog->addEvl(&fib, "Fib");

  // Define and start HealthChecker
  std::unique_ptr<HealthChecker> healthChecker{nullptr};
  if (FLAGS_enable_health_checker) {
    healthChecker = std::make_unique<HealthChecker>(
        FLAGS_node_name,
        openr::thrift::HealthCheckOption(FLAGS_health_check_option),
        FLAGS_health_check_pct,
        static_cast<uint16_t>(FLAGS_health_checker_port),
        std::chrono::seconds(FLAGS_health_checker_ping_interval_s),
        maybeIpTos,
        AdjacencyDbMarker{Constants::kAdjDbMarker.toString()},
        PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
        kvStoreLocalCmdUrl,
        kvStoreLocalPubUrl,
        HealthCheckerCmdUrl{folly::sformat(
          "tcp://{}:{}", FLAGS_listen_addr, FLAGS_health_checker_rep_port)},
        monitorSubmitUrl,
        context);
    // Spawn a HealthChecker thread
    allThreads.emplace_back(std::thread([&healthChecker]() noexcept {
      LOG(INFO) << "Starting HealthChecker thread ...";
      folly::setThreadName("HealthChecker");
      healthChecker->run();
      LOG(INFO) << "HealthChecker thread got stopped.";
    }));
    healthChecker->waitUntilRunning();
    watchdog->addEvl(healthChecker.get(), "HealthChecker");
  }

  LOG(INFO) << "Starting main event loop...";
  mainEventLoop.run();
  LOG(INFO) << "Main event loop got stopped";

  // Stop all threads (in reverse order of their creation)
  if (healthChecker) {
    healthChecker->stop();
    healthChecker->waitUntilStopped();
  }
  fib.stop();
  fib.waitUntilStopped();
  decision.stop();
  decision.waitUntilStopped();
  linkMonitor.stop();
  linkMonitor.waitUntilStopped();
  spark.stop();
  spark.waitUntilStopped();
  if (prefixAllocator) {
    prefixAllocator->stop();
    prefixAllocator->waitUntilStopped();
  }
  prefixManager.stop();
  prefixManager.waitUntilStopped();
  store.stop();
  store.waitUntilStopped();
  monitor.stop();
  monitor.waitUntilStopped();
  configStore.stop();
  configStore.waitUntilStopped();
  if (netlinkFibServer) {
    netlinkFibServer->stop();
  }
  if (netlinkSystemServer) {
    netlinkSystemServer->stop();
  }
  if (thriftThreadMgr) {
    thriftThreadMgr->stop();
  }
  if (watchdog) {
    watchdog->stop();
    watchdog->waitUntilStopped();
  }

  // Wait for all threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  // Close syslog connection (this is optional)
  syslog(LOG_NOTICE, "Stopping OpenR daemon.");
  closelog();

  return 0;
}
