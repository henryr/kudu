// Copyright (c) 2013, Cloudera, inc.
// All rights reserved.

#include <boost/thread/shared_mutex.hpp>
#include <string>
#include <tr1/memory>

#include "gutil/strings/strip.h"
#include "util/env.h"
#include "util/env_util.h"
#include "util/status.h"
#include "tablet/deltafile.h"
#include "tablet/delta_store.h"
#include "tablet/diskrowset.h"

namespace kudu { namespace tablet {

using std::string;
using std::tr1::shared_ptr;


namespace {

// DeltaIterator that simply combines together other DeltaIterators,
// applying deltas from each in order.
class DeltaIteratorMerger : public DeltaIterator {
 public:
  // Create a new DeltaIterator which combines the deltas from
  // all of the input delta stores.
  //
  // If only one store is input, this will automatically return an unwrapped
  // iterator for greater efficiency.
  static shared_ptr<DeltaIterator> Create(
    const vector<shared_ptr<DeltaStore> > &stores,
    const Schema &projection,
    const MvccSnapshot &snapshot);

  ////////////////////////////////////////////////////////////
  // Implementations of DeltaIterator
  ////////////////////////////////////////////////////////////
  virtual Status Init();
  virtual Status SeekToOrdinal(rowid_t idx);
  virtual Status PrepareBatch(size_t nrows);
  virtual Status ApplyUpdates(size_t col_to_apply, ColumnBlock *dst);
  virtual Status ApplyDeletes(SelectionVector *sel_vec);
  virtual Status CollectMutations(vector<Mutation *> *dst, Arena *arena);
  virtual string ToString() const;

 private:
  explicit DeltaIteratorMerger(const vector<shared_ptr<DeltaIterator> > &iters);

  vector<shared_ptr<DeltaIterator> > iters_;
};

DeltaIteratorMerger::DeltaIteratorMerger(const vector<shared_ptr<DeltaIterator> > &iters) :
  iters_(iters)
{}

Status DeltaIteratorMerger::Init() {
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    RETURN_NOT_OK(iter->Init());
  }
  return Status::OK();
}

Status DeltaIteratorMerger::SeekToOrdinal(rowid_t idx) {
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    RETURN_NOT_OK(iter->SeekToOrdinal(idx));
  }
  return Status::OK();
}

Status DeltaIteratorMerger::PrepareBatch(size_t nrows) {
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    RETURN_NOT_OK(iter->PrepareBatch(nrows));
  }
  return Status::OK();
}

Status DeltaIteratorMerger::ApplyUpdates(size_t col_to_apply, ColumnBlock *dst) {
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    RETURN_NOT_OK(iter->ApplyUpdates(col_to_apply, dst));
  }
  return Status::OK();
}

Status DeltaIteratorMerger::ApplyDeletes(SelectionVector *sel_vec) {
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    RETURN_NOT_OK(iter->ApplyDeletes(sel_vec));
  }
  return Status::OK();
}

Status DeltaIteratorMerger::CollectMutations(vector<Mutation *> *dst, Arena *arena) {
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    RETURN_NOT_OK(iter->CollectMutations(dst, arena));
  }
  // TODO: do we need to do some kind of sorting here to deal with out-of-order
  // txids?
  return Status::OK();
}


string DeltaIteratorMerger::ToString() const {
  string ret;
  ret.append("DeltaIteratorMerger(");

  bool first = true;
  BOOST_FOREACH(const shared_ptr<DeltaIterator> &iter, iters_) {
    if (!first) {
      ret.append(", ");
    }
    first = false;

    ret.append(iter->ToString());
  }
  ret.append(")");
  return ret;
}


shared_ptr<DeltaIterator> DeltaIteratorMerger::Create(
  const vector<shared_ptr<DeltaStore> > &stores,
  const Schema &projection,
  const MvccSnapshot &snapshot)
{
  vector<shared_ptr<DeltaIterator> > delta_iters;

  BOOST_FOREACH(const shared_ptr<DeltaStore> &store, stores) {
    shared_ptr<DeltaIterator> iter(store->NewDeltaIterator(projection, snapshot));
    delta_iters.push_back(iter);
  }

  if (delta_iters.size() == 1) {
    // If we only have one input to the "merge", we can just directly
    // return that iterator.
    return delta_iters[0];
  }

  return shared_ptr<DeltaIterator>(new DeltaIteratorMerger(delta_iters));
}

} // anonymous namespace


DeltaTracker::DeltaTracker(Env *env,
                           const Schema &schema,
                           const string &dir,
                           rowid_t num_rows) :
  env_(env),
  schema_(schema),
  dir_(dir),
  num_rows_(num_rows),
  open_(false),
  next_deltafile_idx_(0),
  dms_(new DeltaMemStore(schema))
{}


// Open any previously flushed DeltaFiles in this rowset
Status DeltaTracker::Open() {
  CHECK(delta_stores_.empty()) << "should call before opening any readers";
  CHECK(!open_);

  vector<string> children;
  RETURN_NOT_OK(env_->GetChildren(dir_, &children));
  BOOST_FOREACH(const string &child, children) {
    // Skip hidden files (also '.' and '..')
    if (child[0] == '.') continue;

    string absolute_path = env_->JoinPathSegments(dir_, child);

    string suffix;
    if (TryStripPrefixString(child, DiskRowSet::kDeltaPrefix, &suffix)) {
      // The file should be named 'delta_<N>'. N here is the index
      // of the delta file (indicating the order in which it was flushed).
      uint32_t deltafile_idx;
      if (!safe_strtou32(suffix.c_str(), &deltafile_idx)) {
        return Status::IOError(string("Bad delta file: ") + absolute_path);
      }

      gscoped_ptr<DeltaFileReader> dfr;
      Status s = DeltaFileReader::Open(env_, absolute_path, schema_, &dfr);
      if (!s.ok()) {
        LOG(ERROR) << "Failed to open delta file " << absolute_path << ": "
                   << s.ToString();
        return s;
      }
      LOG(INFO) << "Successfully opened delta file " << absolute_path;

      delta_stores_.push_back(shared_ptr<DeltaStore>(dfr.release()));

      next_deltafile_idx_ = std::max(next_deltafile_idx_,
                                     deltafile_idx + 1);
    } else if (TryStripPrefixString(child, DiskRowSet::kColumnPrefix, &suffix)) {
      // expected: column data
    } else {
      LOG(WARNING) << "ignoring unknown file: " << absolute_path;
    }
  }

  return Status::OK();
}


void DeltaTracker::CollectStores(vector<shared_ptr<DeltaStore> > *deltas) const {
  boost::lock_guard<boost::shared_mutex> lock(component_lock_);
  deltas->assign(delta_stores_.begin(), delta_stores_.end());
  deltas->push_back(dms_);
}

shared_ptr<DeltaIterator> DeltaTracker::NewDeltaIterator(const Schema &schema,
                                                                  const MvccSnapshot &snap) const {
  std::vector<shared_ptr<DeltaStore> > stores;
  CollectStores(&stores);
  return DeltaIteratorMerger::Create(stores, schema, snap);
}

ColumnwiseIterator *DeltaTracker::WrapIterator(const shared_ptr<ColumnwiseIterator> &base,
                                               const MvccSnapshot &mvcc_snap) const
{
  return new DeltaApplier(base, NewDeltaIterator(base->schema(), mvcc_snap));
}


void DeltaTracker::Update(txid_t txid, rowid_t row_idx, const RowChangeList &update) {
  // TODO: can probably lock this more fine-grained.
  boost::shared_lock<boost::shared_mutex> lock(component_lock_);

  DCHECK_LT(row_idx, num_rows_);

  dms_->Update(txid, row_idx, update);
}

Status DeltaTracker::CheckRowDeleted(rowid_t row_idx, bool *deleted) const {
  boost::shared_lock<boost::shared_mutex> lock(component_lock_);

  DCHECK_LT(row_idx, num_rows_);

  *deleted = false;
  // Check if the row has a deletion in DeltaMemStore.
  RETURN_NOT_OK(dms_->CheckRowDeleted(row_idx, deleted));
  if (*deleted) {
    return Status::OK();
  }

  // Then check backwards through the list of trackers.
  BOOST_REVERSE_FOREACH(const shared_ptr<DeltaStore> &ds, delta_stores_) {
    RETURN_NOT_OK(ds->CheckRowDeleted(row_idx, deleted));
    if (*deleted) {
      return Status::OK();
    }
  }

  return Status::OK();
}

Status DeltaTracker::FlushDMS(const DeltaMemStore &dms,
                              gscoped_ptr<DeltaFileReader> *dfr) {
  int deltafile_idx = next_deltafile_idx_++;
  string path = DiskRowSet::GetDeltaPath(dir_, deltafile_idx);

  // Open file for write.
  shared_ptr<WritableFile> out;
  Status s = env_util::OpenFileForWrite(env_, path, &out);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to open output file for delta level " <<
      deltafile_idx << " at path " << path << ": " << 
      s.ToString();
    return s;
  }
  DeltaFileWriter dfw(schema_, out);

  s = dfw.Start();
  if (!s.ok()) {
    LOG(WARNING) << "Unable to start delta file writer for path " <<
      path;
    return s;
  }
  RETURN_NOT_OK(dms.FlushToFile(&dfw));
  RETURN_NOT_OK(dfw.Finish());
  LOG(INFO) << "Flushed delta file: " << path;

  // Now re-open for read
  RETURN_NOT_OK(DeltaFileReader::Open(env_, path, schema_, dfr));
  LOG(INFO) << "Reopened delta file for read: " << path;

  return Status::OK();
}

Status DeltaTracker::Flush() {
  // First, swap out the old DeltaMemStore with a new one,
  // and add it to the list of delta stores to be reflected
  // in reads.
  shared_ptr<DeltaMemStore> old_dms;
  size_t count;
  {
    // Lock the component_lock_ in exclusive mode.
    // This shuts out any concurrent readers or writers.
    boost::lock_guard<boost::shared_mutex> lock(component_lock_);

    count = dms_->Count();
    if (count == 0) {
      // No need to flush if there are no deltas.
      return Status::OK();
    }

    old_dms = dms_;
    dms_.reset(new DeltaMemStore(schema_));

    delta_stores_.push_back(old_dms);
  }

  LOG(INFO) << "Flushing " << count << " deltas...";

  // Now, actually flush the contents of the old DMS.
  // TODO: need another lock to prevent concurrent flushers
  // at some point.
  gscoped_ptr<DeltaFileReader> dfr;
  Status s = FlushDMS(*old_dms, &dfr);
  CHECK(s.ok())
    << "Failed to flush DMS: " << s.ToString()
    << "\nTODO: need to figure out what to do with error handling "
    << "if this fails -- we end up with a DeltaMemStore permanently "
    << "in the store list. For now, abort.";


  // Now, re-take the lock and swap in the DeltaFileReader in place of
  // of the DeltaMemStore
  {
    boost::lock_guard<boost::shared_mutex> lock(component_lock_);
    size_t idx = delta_stores_.size() - 1;

    CHECK_EQ(delta_stores_[idx], old_dms)
      << "Another thread modified the delta store list during flush";
    delta_stores_[idx].reset(dfr.release());
  }

  return Status::OK();

  // TODO: wherever we write stuff, we should write to a tmp path
  // and rename to final path!
}

////////////////////////////////////////////////////////////
// Delta merger
////////////////////////////////////////////////////////////

} // namespace tablet
} // namespace kudu
