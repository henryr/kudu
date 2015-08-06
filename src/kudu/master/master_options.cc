// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include "kudu/master/master_options.h"

#include <gflags/gflags.h>

#include "kudu/gutil/strings/split.h"
#include "kudu/master/master.h"
#include "kudu/util/env.h"

namespace kudu {
namespace master {

DEFINE_string(master_wal_dir, "/tmp/kudu-master",
              "Directory where the Master will place its write-ahead logs. "
              "May be the same as --master_data_dirs");
DEFINE_string(master_data_dirs, "/tmp/kudu-master",
              "Comma-separated list of directories where the Master will "
              "place its data blocks");

DEFINE_string(master_rpc_bind_addresses, "0.0.0.0:7051",
              "Comma-separated list of addresses for the Master to bind "
              "to for RPC connections");

DEFINE_string(master_addresses, "",
              "Comma-separated list of all the RPC addresses for Master config."
              " NOTE: if not specified, assumes a standalone Master.");

DEFINE_int32(master_web_port, Master::kDefaultWebPort,
             "Port to bind to for the Master web server");
DEFINE_int32(master_num_acceptors_per_address, 1,
             "Number of RPC acceptor threads for each bound address");
DEFINE_int32(master_num_service_threads, 10,
             "Number of RPC worker threads to run");


MasterOptions::MasterOptions() {
  rpc_opts.rpc_bind_addresses = FLAGS_master_rpc_bind_addresses;
  rpc_opts.num_acceptors_per_address = FLAGS_master_num_acceptors_per_address;
  rpc_opts.num_service_threads = FLAGS_master_num_service_threads;
  rpc_opts.default_port = Master::kDefaultPort;

  webserver_opts.port = FLAGS_master_web_port;
  // The rest of the web server options are not overridable on a per-tablet-server
  // basis.

  wal_dir = FLAGS_master_wal_dir;
  data_dirs = strings::Split(FLAGS_master_data_dirs, ",",
                             strings::SkipEmpty());

  env = Env::Default();

  if (!FLAGS_master_addresses.empty()) {
    Status s = HostPort::ParseStrings(FLAGS_master_addresses, Master::kDefaultPort,
                                      &master_addresses);
    if (!s.ok()) {
      LOG(FATAL) << "Couldn't parse the master_addresses flag('" << FLAGS_master_addresses << "'): "
                 << s.ToString();
    }
    if (master_addresses.size() < 2) {
      LOG(FATAL) << "At least 2 masters are required for a distributed config, but "
          "master_addresses flag ('" << FLAGS_master_addresses << "') only specifies "
                 << master_addresses.size() << " masters.";
    }
    if (master_addresses.size() == 2) {
      LOG(WARNING) << "Only 2 masters are specified by master_addresses_flag ('" <<
          FLAGS_master_addresses << "'), but minimum of 3 are required to tolerate failures"
          " of any one master. It is recommended to use at least 3 masters.";
    }
  }
}

bool MasterOptions::IsDistributed() const {
  return !master_addresses.empty();
}

} // namespace master
} // namespace kudu