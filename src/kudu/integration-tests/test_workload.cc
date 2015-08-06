// Copyright (c) 2015, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <boost/foreach.hpp>

#include "kudu/client/client.h"
#include "kudu/client/client-test-util.h"
#include "kudu/client/schema-internal.h"
#include "kudu/common/schema.h"
#include "kudu/common/wire_protocol-test-util.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/integration-tests/external_mini_cluster.h"
#include "kudu/integration-tests/test_workload.h"
#include "kudu/util/env.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/random.h"
#include "kudu/util/thread.h"

namespace kudu {

using client::FromInternalCompressionType;
using client::FromInternalDataType;
using client::FromInternalEncodingType;
using client::KuduClient;
using client::KuduClientBuilder;
using client::KuduColumnSchema;;
using client::KuduInsert;
using client::KuduSchema;
using client::KuduSchemaBuilder;
using client::KuduSession;
using client::KuduTable;
using client::KuduTableCreator;
using std::tr1::shared_ptr;

const char* const TestWorkload::kDefaultTableName = "test-workload";

TestWorkload::TestWorkload(ExternalMiniCluster* cluster)
  : cluster_(cluster),
    num_write_threads_(4),
    write_batch_size_(50),
    write_timeout_millis_(20000),
    timeout_allowed_(false),
    not_found_allowed_(false),
    num_replicas_(3),
    table_name_(kDefaultTableName),
    start_latch_(0),
    should_run_(false),
    rows_inserted_(0),
    batches_completed_(0) {
}

TestWorkload::~TestWorkload() {
}

void TestWorkload::WriteThread() {
  Random r(Env::Default()->gettid());

  shared_ptr<KuduTable> table;
  // Loop trying to open up the table. In some tests we set up very
  // low RPC timeouts to test those behaviors, so this might fail and
  // need retrying.
  while (should_run_.Load()) {
    Status s = client_->OpenTable(table_name_, &table);
    if (s.ok()) {
      break;
    }
    if (timeout_allowed_ && s.IsTimedOut()) {
      SleepFor(MonoDelta::FromMilliseconds(50));
      continue;
    }
    CHECK_OK(s);
  }

  shared_ptr<KuduSession> session = client_->NewSession();
  session->SetTimeoutMillis(write_timeout_millis_);
  CHECK_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  // Wait for all of the workload threads to be ready to go. This maximizes the chance
  // that they all send a flood of requests at exactly the same time.
  //
  // This also minimizes the chance that we see failures to call OpenTable() if
  // a late-starting thread overlaps with the flood of outbound traffic from the
  // ones that are already writing data.
  start_latch_.CountDown();
  start_latch_.Wait();

  while (should_run_.Load()) {
    for (int i = 0; i < write_batch_size_; i++) {
      gscoped_ptr<KuduInsert> insert(table->NewInsert());
      KuduPartialRow* row = insert->mutable_row();
      CHECK_OK(row->SetInt32(0, r.Next()));
      CHECK_OK(row->SetInt32(1, r.Next()));
      CHECK_OK(row->SetStringCopy(2, "hello world"));
      CHECK_OK(session->Apply(insert.release()));
    }

    int inserted = write_batch_size_;

    Status s = session->Flush();

    if (PREDICT_FALSE(!s.ok())) {
      std::vector<client::KuduError*> errors;
      ElementDeleter d(&errors);
      bool overflow;
      session->GetPendingErrors(&errors, &overflow);
      CHECK(!overflow);
      BOOST_FOREACH(const client::KuduError* e, errors) {
        if (timeout_allowed_ && e->status().IsTimedOut()) {
          continue;
        }

        if (not_found_allowed_ && e->status().IsNotFound()) {
          continue;
        }
        // We don't handle write idempotency yet. (i.e making sure that when a leader fails
        // writes to it that were eventually committed by the new leader but un-ackd to the
        // client are not retried), so some errors are expected.
        // It's OK as long as the errors are Status::AlreadyPresent();
        CHECK(e->status().IsAlreadyPresent()) << "Unexpected error: " << e->status().ToString();
      }
      inserted -= errors.size();
    }

    rows_inserted_.IncrementBy(inserted);
    if (inserted > 0) {
      batches_completed_.Increment();
    }
  }
}

void TestWorkload::Setup() {
  CHECK_OK(cluster_->CreateClient(client_builder_, &client_));

  bool exists;
  CHECK_OK(client_->TableExists(table_name_, &exists));
  if (exists) {
    LOG(INFO) << "TestWorkload: Skipping table creation because table "
              << table_name_ << " already exists";
    return;
  }
  KuduSchema client_schema(GetSimpleTestSchema());

  gscoped_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
  CHECK_OK(table_creator->table_name(table_name_)
           .schema(&client_schema)
           .num_replicas(num_replicas_)
           // NOTE: this is quite high as a timeout, but the default (5 sec) does not
           // seem to be high enough in some cases (see KUDU-550). We should remove
           // this once that ticket is addressed.
           .timeout(MonoDelta::FromSeconds(20))
           .Create());
}

void TestWorkload::Start() {
  CHECK(!should_run_.Load()) << "Already started";
  should_run_.Store(true);
  start_latch_.Reset(num_write_threads_);
  for (int i = 0; i < num_write_threads_; i++) {
    scoped_refptr<kudu::Thread> new_thread;
    CHECK_OK(kudu::Thread::Create("test", strings::Substitute("test-writer-$0", i),
                                  &TestWorkload::WriteThread, this,
                                  &new_thread));
    threads_.push_back(new_thread);
  }
}

void TestWorkload::StopAndJoin() {
  should_run_.Store(false);
  start_latch_.Reset(0);
  BOOST_FOREACH(scoped_refptr<kudu::Thread> thr, threads_) {
   CHECK_OK(ThreadJoiner(thr.get()).Join());
  }
  threads_.clear();
}

} // namespace kudu