/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <map>
#include <string>

#include "Version.h"

#if OSQUERY_VERSION_NUMBER < SDK_VERSION(4, 0)
#include <osquery/sdk.h>

static inline void insertRow(osquery::TableRows &result, osquery::Row &row) {
  result.push_back(row);
}
#else
#include <osquery/sdk/sdk.h>
#include <osquery/sql/dynamic_table_row.h>

static inline void insertRow(osquery::TableRows &result, osquery::Row &row) {
  result.push_back(osquery::TableRowHolder(new osquery::DynamicTableRow(std::move(row))));
}
#endif

namespace trailofbits {
/// This is the table plugin for ntfs_indx_data
class NTFSINDXTablePugin final : public osquery::TablePlugin {
 public:
  /// Returns the table schema
  osquery::TableColumns columns() const override;

  /// Generates the partition list
  osquery::TableRows generate(osquery::QueryContext& context) override;
};
}

// Export the class outside the namespace so that osquery can pick it up
using NTFSINDXTablePugin = trailofbits::NTFSINDXTablePugin;
