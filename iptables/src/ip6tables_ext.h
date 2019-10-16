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

#if OSQUERY_VERSION_NUMBER < OSQUERY_SDK_VERSION(4, 0)
#include <osquery/sdk.h>
#else
#include <osquery/sdk/sdk.h>
#endif

extern "C" {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Werror"
#include <libiptc/libip6tc.h>
#pragma clang diagnostic pop
}

#include "utils.h"

namespace trailofbits {
class Ip6tablesExtTable : public IptablesExtBase {
 public:
#if OSQUERY_VERSION_NUMBER < OSQUERY_SDK_VERSION(4, 0)
  osquery::QueryData generate(osquery::QueryContext& context);
#else
  osquery::TableRows generate(osquery::QueryContext& context);
#endif
 private:
  osquery::Status genIptablesRules(const std::string& filter,
                                   const MatchChain& matches,
#if OSQUERY_VERSION_NUMBER < OSQUERY_SDK_VERSION(4, 0)
                                   osquery::QueryData& results);
#else
                                   osquery::TableRows& results);
#endif
  void parseTcp(const xt_entry_match* match, osquery::Row& r);
  void parseUdp(const xt_entry_match* match, osquery::Row& r);
  void parseIpEntry(const ip6t_ip6* ip, osquery::Row& r);
};
} // namespace trailofbits

using Ip6tablesExtTable = trailofbits::Ip6tablesExtTable;
