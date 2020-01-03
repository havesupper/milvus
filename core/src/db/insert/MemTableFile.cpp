// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "db/insert/MemTableFile.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "db/Constants.h"
#include "db/engine/EngineFactory.h"
#include "metrics/Metrics.h"
#include "utils/Log.h"
#include "utils/ValidationUtil.h"

namespace milvus {
namespace engine {

MemTableFile::MemTableFile(const std::string& table_id, const meta::MetaPtr& meta, const DBOptions& options)
    : table_id_(table_id), meta_(meta), options_(options) {
    current_mem_ = 0;
    auto status = CreateTableFile();
    if (status.ok()) {
        /*execution_engine_ = EngineFactory::Build(
            table_file_schema_.dimension_, table_file_schema_.location_, (EngineType)table_file_schema_.engine_type_,
            (MetricType)table_file_schema_.metric_type_, table_file_schema_.nlist_);*/
        segment_writer_ptr_ = std::make_shared<segment::SegmentWriter>(table_file_schema_.directory_);
    }
}

Status
MemTableFile::CreateTableFile() {
    meta::TableFileSchema table_file_schema;
    table_file_schema.table_id_ = table_id_;
    auto status = meta_->CreateTableFile(table_file_schema);
    if (status.ok()) {
        table_file_schema_ = table_file_schema;
    } else {
        std::string err_msg = "MemTableFile::CreateTableFile failed: " + status.ToString();
        ENGINE_LOG_ERROR << err_msg;
    }
    return status;
}

Status
MemTableFile::Add(VectorSourcePtr& source) {
    if (table_file_schema_.dimension_ <= 0) {
        std::string err_msg =
            "MemTableFile::Add: table_file_schema dimension = " + std::to_string(table_file_schema_.dimension_) +
            ", table_id = " + table_file_schema_.table_id_;
        ENGINE_LOG_ERROR << err_msg;
        return Status(DB_ERROR, "Not able to create table file");
    }

    size_t single_vector_mem_size = source->SingleVectorSize(table_file_schema_.dimension_);
    size_t mem_left = GetMemLeft();
    if (mem_left >= single_vector_mem_size) {
        size_t num_vectors_to_add = std::ceil(mem_left / single_vector_mem_size);
        size_t num_vectors_added;
        auto status = source->Add(/*execution_engine_,*/ segment_writer_ptr_, table_file_schema_, num_vectors_to_add,
                                  num_vectors_added);
        if (status.ok()) {
            current_mem_ += (num_vectors_added * single_vector_mem_size);
        }
        return status;
    }
    return Status::OK();
}

Status
MemTableFile::Delete(segment::doc_id_t doc_id) {
    // Check wither the doc_id is present, if yes, delete it's corresponding buffer

    // TODO: need to know the type of vector we want to delete. Hard code for now
    int vector_type_size;
    if (server::ValidationUtil::IsBinaryMetricType(table_file_schema_.metric_type_)) {
        vector_type_size = sizeof(uint8_t);
    } else {
        vector_type_size = sizeof(float);
    }

    segment::SegmentPtr segment_ptr;
    segment_writer_ptr_->GetSegment(segment_ptr);
    auto vectors_map = segment_ptr->vectors_ptr_->vectors;
    for (auto& it : vectors_map) {
        auto uids = it.second->GetUids();
        auto found = std::find(uids.begin(), uids.end(), doc_id);
        if (found != uids.end()) {
            auto offset = std::distance(uids.begin(), found);
            it.second->Erase(offset, vector_type_size);
        }
    }

    // Then add the id to delete queue so it can be applied to segment on disk during the next flush
    segment_ptr->deleted_docs_ptr_->AddDeleteDoc(doc_id);

    return Status::OK();
}

size_t
MemTableFile::GetCurrentMem() {
    return current_mem_;
}

size_t
MemTableFile::GetMemLeft() {
    return (MAX_TABLE_FILE_MEM - current_mem_);
}

bool
MemTableFile::IsFull() {
    size_t single_vector_mem_size = table_file_schema_.dimension_ * FLOAT_TYPE_SIZE;
    return (GetMemLeft() < single_vector_mem_size);
}

Status
MemTableFile::Serialize() {
    size_t size = GetCurrentMem();
    server::CollectSerializeMetrics metrics(size);

    segment_writer_ptr_->Serialize();

    //    execution_engine_->Serialize();

    // TODO:
    //    table_file_schema_.file_size_ = execution_engine_->PhysicalSize();
    //    table_file_schema_.row_count_ = execution_engine_->Count();

    // if index type isn't IDMAP, set file type to TO_INDEX if file size exceed index_file_size
    // else set file type to RAW, no need to build index
    if (table_file_schema_.engine_type_ != (int)EngineType::FAISS_IDMAP &&
        table_file_schema_.engine_type_ != (int)EngineType::FAISS_BIN_IDMAP) {
        table_file_schema_.file_type_ = (size >= table_file_schema_.index_file_size_) ? meta::TableFileSchema::TO_INDEX
                                                                                      : meta::TableFileSchema::RAW;
    } else {
        table_file_schema_.file_type_ = meta::TableFileSchema::RAW;
    }

    auto status = meta_->UpdateTableFile(table_file_schema_);

    ENGINE_LOG_DEBUG << "New " << ((table_file_schema_.file_type_ == meta::TableFileSchema::RAW) ? "raw" : "to_index")
                     << " file " << table_file_schema_.file_id_ << " of size " << size << " bytes";

    // TODO: cache
    /*
        if (options_.insert_cache_immediately_) {
            execution_engine_->Cache();
        }
    */
    if (options_.insert_cache_immediately_) {
        segment_writer_ptr_->Cache();
    }

    return status;
}

}  // namespace engine
}  // namespace milvus
