// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef ROCKSDB_LITE

#include "table/rtree_table_reader.h"

#include <string>
#include <vector>

#include "db/dbformat.h"

#include "rocksdb/cache.h"
#include "rocksdb/comparator.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"

#include "table/block.h"
#include "table/bloom_block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/internal_iterator.h"
#include "table/meta_blocks.h"
#include "table/two_level_iterator.h"
#include "table/rtree_table_factory.h"
#include "table/get_context.h"

#include "util/arena.h"
#include "util/coding.h"
#include "util/dynamic_bloom.h"
#include "util/hash.h"
#include "util/histogram.h"
#include "util/murmurhash.h"
#include "util/perf_context_imp.h"
#include "util/stop_watch.h"
#include "util/string_util.h"


namespace rocksdb {

namespace {

// Safely getting a uint32_t element from a char array, where, starting from
// `base`, every 4 bytes are considered as an fixed 32 bit integer.
inline uint32_t GetFixed32Element(const char* base, size_t offset) {
  return DecodeFixed32(base + offset * sizeof(uint32_t));
}
}  // namespace

// Iterator to iterate IndexedTable
class RtreeTableIterator : public InternalIterator {
 public:
  explicit RtreeTableIterator(RtreeTableReader* table);
  ~RtreeTableIterator();

  bool Valid() const override;

  void SeekToFirst() override;

  void SeekToLast() override;

  void Seek(const Slice& target) override;

  void SeekForPrev(const Slice& target) override;

  void Next() override;

  void Prev() override;

  Slice key() const override;

  Slice value() const override;

  Status status() const override;

 private:
  RtreeTableReader* table_;
  uint32_t offset_;
  uint32_t next_offset_;
  Slice key_;
  Slice value_;
  Status status_;
  // No copying allowed
  RtreeTableIterator(const RtreeTableIterator&) = delete;
  void operator=(const Iterator&) = delete;
};

extern const uint64_t kRtreeTableMagicNumber;
RtreeTableReader::RtreeTableReader(const ImmutableCFOptions& ioptions,
                                   unique_ptr<RandomAccessFileReader>&& file,
                                   const EnvOptions& storage_options,
                                   const InternalKeyComparator& icomparator,
                                   uint64_t file_size,
                                   const TableProperties* table_properties)
    : internal_comparator_(icomparator),
      file_info_(std::move(file), storage_options,
                 static_cast<uint32_t>(table_properties->data_size)),
      ioptions_(ioptions),
      file_size_(file_size),
      table_properties_(nullptr) {}

RtreeTableReader::~RtreeTableReader() {
}

Status RtreeTableReader::Open(const ImmutableCFOptions& ioptions,
                              const EnvOptions& env_options,
                              const InternalKeyComparator& internal_comparator,
                              unique_ptr<RandomAccessFileReader>&& file,
                              uint64_t file_size,
                              unique_ptr<TableReader>* table_reader) {
  TableProperties* props = nullptr;
  auto s = ReadTableProperties(file.get(), file_size, kRtreeTableMagicNumber,
                               ioptions, &props);
  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<RtreeTableReader> new_reader(new RtreeTableReader(
      ioptions, std::move(file), env_options, internal_comparator,
      file_size, props));

  s = new_reader->MmapDataIfNeeded();
  if (!s.ok()) {
    return s;
  }

  *table_reader = std::move(new_reader);
  return s;
}

void RtreeTableReader::SetupForCompaction() {
}

InternalIterator* RtreeTableReader::NewIterator(const ReadOptions& options,
                                                Arena* arena,
                                                bool skip_filters) {
  if (arena == nullptr) {
    return new RtreeTableIterator(this);
  } else {
    auto mem = arena->AllocateAligned(sizeof(RtreeTableIterator));
    return new (mem) RtreeTableIterator(this);
  }
}


Status RtreeTableReader::MmapDataIfNeeded() {
  if (file_info_.is_mmap_mode) {
    // Get mmapped memory.
    return file_info_.file->Read(0, file_size_, &file_info_.file_data, nullptr);
  }
  return Status::OK();
}

Status RtreeTableReader::Next(uint32_t* offset,
                              ParsedInternalKey* parsed_key,
                              Slice* internal_key,
                              Slice* value) const {
//  if (offset >= file_size_) {
//    return file_size_;
//  }
//  Status s = file_->Read(offset, internal_key.size(), key, nullptr);
//  offset += internal_key.size();
// 
//  s = file_->Read(offset, 4, tmp_slice, nullptr);
//  offset += 4;
//  uint32_t value_size = DecodeFixed32(tmp_slice->data());
// 
//  s = file_->Read(offset, value_size, value, nullptr);
//  offset += value_size;

  //  return offset;
  // XXX vmx 2016-12-13: implement  the actual iterator
  return Status::OK();
}

void RtreeTableReader::Prepare(const Slice& target) {
}

Status RtreeTableReader::Get(const ReadOptions& ro, const Slice& target,
                             GetContext* get_context, bool skip_filters) {
  uint32_t offset = 0;

  ParsedInternalKey found_key;
  ParsedInternalKey parsed_target;
  if (!ParseInternalKey(target, &parsed_target)) {
    return Status::Corruption(Slice());
  }
  Slice found_value;
  while (offset < file_info_.data_end_offset) {
    Status s = Next(&offset, &found_key, nullptr, &found_value);
    if (!s.ok()) {
      return s;
    }
    if (internal_comparator_.Compare(found_key, parsed_target) >= 0) {
      if (!get_context->SaveValue(found_key, found_value)) {
        break;
      }
    }
  }
  return Status::OK();
}

uint64_t RtreeTableReader::ApproximateOffsetOf(const Slice& key) {
  return 0;
}

RtreeTableIterator::RtreeTableIterator(RtreeTableReader* table)
    : table_(table) {
  next_offset_ = offset_ = table_->file_info_.data_end_offset;
}

RtreeTableIterator::~RtreeTableIterator() {
}

bool RtreeTableIterator::Valid() const {
  return offset_ < table_->file_info_.data_end_offset &&
         offset_ >= table_->data_start_offset_;
}

void RtreeTableIterator::SeekToFirst() {
  next_offset_ = table_->data_start_offset_;
  if (next_offset_ >= table_->file_info_.data_end_offset) {
    next_offset_ = offset_ = table_->file_info_.data_end_offset;
  } else {
    Next();
  }
}

void RtreeTableIterator::SeekToLast() {
  assert(false);
  status_ = Status::NotSupported("SeekToLast() is not supported in RtreeTable");
}

void RtreeTableIterator::Seek(const Slice& target) {
  if (next_offset_ < table_->file_info_.data_end_offset) {
    for (Next(); status_.ok() && Valid(); Next()) {
      if (table_->internal_comparator_.Compare(key(), target) >= 0) {
        break;
      }
    }
  } else {
    offset_ = table_->file_info_.data_end_offset;
  }
}

void RtreeTableIterator::SeekForPrev(const Slice& target) {
  assert(false);
  status_ =
      Status::NotSupported("SeekForPrev() is not supported in RtreeTable");
}

void RtreeTableIterator::Next() {
  offset_ = next_offset_;
  if (offset_ < table_->file_info_.data_end_offset) {
    Slice tmp_slice;
    ParsedInternalKey parsed_key;
    status_ =
        table_->Next(&next_offset_, &parsed_key, &key_, &value_);
    if (!status_.ok()) {
      offset_ = next_offset_ = table_->file_info_.data_end_offset;
    }
  }
}

void RtreeTableIterator::Prev() {
  assert(false);
}

Slice RtreeTableIterator::key() const {
  assert(Valid());
  return key_;
}

Slice RtreeTableIterator::value() const {
  assert(Valid());
  return value_;
}

Status RtreeTableIterator::status() const {
  return status_;
}

}  // namespace rocksdb
#endif  // ROCKSDB_LITE
