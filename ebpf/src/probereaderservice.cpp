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

#include "probereaderservice.h"
#include "probes/common/defs.h"

#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>

#include <osquery/logger.h>

namespace trailofbits {
namespace {
bool readEventSlot(std::vector<std::uint64_t>& table_data,
                   int& index,
                   std::size_t cpu_id,
                   ebpf::BPFPercpuArrayTable<std::uint64_t>& event_data_table) {
  table_data.clear();

  auto status = event_data_table.get_value(index, table_data);
  if (status.code() != 0) {
    LOG(ERROR) << "Read has failed: " << status.msg();
    return false;
  }

  if (cpu_id >= table_data.size()) {
    LOG(ERROR) << "Invalid CPU index: " << cpu_id;
    return false;
  }

  INCREMENT_EVENT_DATA_INDEX(index);
  return true;
}

template <typename T>
bool readEventData(T& value,
                   std::vector<std::uint64_t>& table_data,
                   int& index,
                   std::size_t cpu_id,
                   ebpf::BPFPercpuArrayTable<std::uint64_t>& event_data_table) {
  value = {};

  if (!readEventSlot(table_data, index, cpu_id, event_data_table)) {
    return false;
  }

  value = static_cast<T>(table_data[cpu_id]);
  return true;
}

template <typename BufferType>
bool readEventBuffer(BufferType& value,
                     std::vector<std::uint64_t>& table_data,
                     int& index,
                     std::size_t cpu_id,
                     ebpf::BPFPercpuArrayTable<std::uint64_t>& event_data_table,
                     std::size_t string_buffer_size) {
  value.clear();

  union {
    std::uint64_t chunk;
    char chunk_bytes[8U];
  };

  std::size_t i = 0U;
  auto chunk_count = (string_buffer_size / 8U);

  BufferType new_value;
  new_value.reserve(string_buffer_size);

  auto str_index = index;
  INCREMENT_EVENT_DATA_INDEX_BY(index, chunk_count);

  for (i = 0U; i < chunk_count; ++i) {
    if (!readEventData(
            chunk, table_data, str_index, cpu_id, event_data_table)) {
      return false;
    }

    bool terminate = false;

    for (auto c : chunk_bytes) {
      if (std::is_same<BufferType, std::string>::value) {
        if (c == '\0') {
          terminate = true;
          break;
        }
      }

      new_value.push_back(c);
    }

    if (terminate) {
      break;
    }
  }

  value = std::move(new_value);
  return true;
}

constexpr auto readEventString = &readEventBuffer<std::string>;
constexpr auto readEventByteArray = &readEventBuffer<std::vector<std::uint8_t>>;

bool readEventStringList(
    ProbeEvent::StringList& value,
    std::vector<std::uint64_t>& table_data,
    int& index,
    std::size_t cpu_id,
    ebpf::BPFPercpuArrayTable<std::uint64_t>& event_data_table,
    std::size_t string_buffer_size,
    std::size_t string_list_size) {
  value = {};

  for (std::size_t string_index = 0U; string_index < string_list_size;
       string_index++) {
    std::uint64_t next_qword;
    auto temp_index = index;
    if (!readEventData(
            next_qword, table_data, temp_index, cpu_id, event_data_table)) {
      return false;
    }

    if (next_qword == VARARGS_TRUNCATION) {
      value.truncated = true;
      index = temp_index;
      break;

    } else if (next_qword == VARARGS_TERMINATOR) {
      value.truncated = false;
      index = temp_index;
      break;
    }

    std::string str = {};
    if (!readEventString(str,
                         table_data,
                         index,
                         cpu_id,
                         event_data_table,
                         string_buffer_size)) {
      return false;
    }

    value.data.push_back(std::move(str));
  }

  return true;
}
} // namespace

struct ProbeReaderService::PrivateData final {
  eBPFProbe& probe;

  std::size_t string_buffer_size{0U};
  std::size_t string_list_size{0U};
  std::string probe_name;

  bool tracepoint{true};
  std::vector<ManagedTracepointDescriptor> tracepoint_list;
  std::vector<KprobeDescriptor> kprobe_list;

  std::vector<ProbeEvent> probe_event_list;
  std::mutex probe_event_list_mutex;
  std::condition_variable probe_event_list_cv;

  PrivateData(eBPFProbe& probe_) : probe(probe_) {}
};

ProbeReaderService::ProbeReaderService(eBPFProbe& probe,
                                       ManagedTracepointProbe desc)
    : d(new PrivateData(probe)) {
  d->tracepoint = true;
  d->tracepoint_list = desc.tracepoint_list;

  d->probe_name = desc.name;
  d->string_buffer_size = desc.string_buffer_size;
  d->string_list_size = desc.string_list_size;
}

ProbeReaderService::ProbeReaderService(eBPFProbe& probe, KprobeProbe desc)
    : d(new PrivateData(probe)) {
  d->tracepoint = false;
  d->kprobe_list = desc.kprobe_list;
  d->probe_name = desc.name;
}

ProbeReaderService::~ProbeReaderService() {}

osquery::Status ProbeReaderService::initialize() {
  return osquery::Status(0);
}

osquery::Status ProbeReaderService::configure(const json11::Json&) {
  return osquery::Status(0);
}

void ProbeReaderService::release() {}

void ProbeReaderService::run() {
  while (!shouldTerminate()) {
    auto perf_event_data = d->probe.getPerfEventData();
    processPerfEvents(perf_event_data);
  }
}

ProbeEventList ProbeReaderService::getProbeEvents() {
  ProbeEventList probe_event_list;

  std::unique_lock<std::mutex> lock(d->probe_event_list_mutex);

  if (d->probe_event_list_cv.wait_for(lock, std::chrono::seconds(1)) ==
      std::cv_status::no_timeout) {
    probe_event_list = std::move(d->probe_event_list);
    d->probe_event_list.clear();
  }

  return probe_event_list;
}

void ProbeReaderService::processPerfEvents(
    const std::vector<std::uint32_t>& perf_event_data) {
  auto event_data_table = d->probe.eventDataTable();

  ProbeEventList new_event_list;

  std::size_t probe_list_size;
  if (d->tracepoint) {
    probe_list_size = d->tracepoint_list.size();
  } else {
    probe_list_size = d->kprobe_list.size();
  }

  for (auto event_identifier : perf_event_data) {
    int index =
        static_cast<int>(event_identifier & 0x00FFFFFF) % EVENT_MAP_SIZE;

    auto cpu_id =
        static_cast<std::size_t>((event_identifier >> 28) & 0x000000FF);

    std::vector<std::uint64_t> table_data;

    std::uint64_t event_type;
    if (!readEventData(
            event_type, table_data, index, cpu_id, event_data_table)) {
      continue;
    }

    if ((event_type & 0xFFFFFFFFFFFF0000ULL) != BASE_EVENT_TYPE) {
      LOG(ERROR) << "Broken event type: " << std::hex << event_type
                 << " from the following tracepoint: " << d->probe_name;
      continue;
    }

    event_type &= 0xFFFF;

    if (event_type >= probe_list_size) {
      LOG(ERROR) << "Invalid event type: " << std::hex << event_type
                 << " from the following tracepoint: " << d->probe_name;
      continue;
    }

    if (d->tracepoint) {
      probe_list_size = d->tracepoint_list.size();
    } else {
      probe_list_size = d->kprobe_list.size();
    }

    // Read the header
    ProbeEvent probe_event = {};
    probe_event.event_identifier = event_type;

    if (!readEventData(probe_event.syscall_number,
                       table_data,
                       index,
                       cpu_id,
                       event_data_table)) {
      continue;
    }

    if (!readEventData(probe_event.timestamp,
                       table_data,
                       index,
                       cpu_id,
                       event_data_table)) {
      continue;
    }

    std::uint64_t pid_tgid;
    if (!readEventData(pid_tgid, table_data, index, cpu_id, event_data_table)) {
      continue;
    }

    probe_event.pid = static_cast<pid_t>(pid_tgid & 0xFFFFFFFF);
    probe_event.tgid = static_cast<pid_t>(pid_tgid >> 32U);

    std::uint64_t uid_gid;
    if (!readEventData(uid_gid, table_data, index, cpu_id, event_data_table)) {
      continue;
    }

    probe_event.uid = static_cast<uid_t>(uid_gid & 0xFFFFFFFF);
    probe_event.gid = static_cast<gid_t>(uid_gid >> 32U);

    bool entry_event = false;
    if (d->tracepoint) {
      entry_event = d->tracepoint_list.at(event_type).entry;
    } else {
      entry_event = d->kprobe_list.at(event_type).entry;
    }

    if (!entry_event) {
      int exit_code;
      if (!readEventData(
              exit_code, table_data, index, cpu_id, event_data_table)) {
        continue;
      }

      probe_event.exit_code = exit_code;
    }

    bool parameter_read_error = false;
    const auto& parameter_list =
        (d->tracepoint ? d->tracepoint_list.at(event_type).parameter_list
                       : d->kprobe_list.at(event_type).parameter_list);

    for (const auto& parameter : parameter_list) {
      if (parameter.type == ProbeParameter::Type::String) {
        std::string value;
        if (!readEventString(value,
                             table_data,
                             index,
                             cpu_id,
                             event_data_table,
                             d->string_buffer_size)) {
          parameter_read_error = true;
          break;
        }

        probe_event.field_list.insert({parameter.name, value});

      } else if (parameter.type == ProbeParameter::Type::SignedInteger) {
        std::int64_t value;
        if (!readEventData(
                value, table_data, index, cpu_id, event_data_table)) {
          parameter_read_error = true;
          break;
        }

        probe_event.field_list.insert({parameter.name, value});

      } else if (parameter.type == ProbeParameter::Type::UnsignedInteger) {
        std::uint64_t value;
        if (!readEventData(
                value, table_data, index, cpu_id, event_data_table)) {
          parameter_read_error = true;
          break;
        }

        probe_event.field_list.insert({parameter.name, value});

      } else if (parameter.type == ProbeParameter::Type::StringList) {
        ProbeEvent::StringList value;
        if (!readEventStringList(value,
                                 table_data,
                                 index,
                                 cpu_id,
                                 event_data_table,
                                 d->string_buffer_size,
                                 d->string_list_size)) {
          parameter_read_error = true;
          break;
        }

        probe_event.field_list.insert({parameter.name, value});

      } else if (parameter.type == ProbeParameter::Type::ByteArray) {
        std::vector<std::uint8_t> value;
        if (!readEventByteArray(value,
                                table_data,
                                index,
                                cpu_id,
                                event_data_table,
                                d->string_buffer_size)) {
          parameter_read_error = true;
          break;
        }

        probe_event.field_list.insert({parameter.name, value});

      } else {
        LOG(ERROR) << "Invalid parameter from the following tracepoint: "
                   << d->probe_name;

        parameter_read_error = true;
        break;
      }
    }

    if (parameter_read_error) {
      continue;
    }

    new_event_list.push_back(std::move(probe_event));
    probe_event = {};
  }

  if (new_event_list.empty()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(d->probe_event_list_mutex);

    d->probe_event_list.reserve(d->probe_event_list.size() +
                                new_event_list.size());

    d->probe_event_list.insert(d->probe_event_list.end(),
                               std::make_move_iterator(new_event_list.begin()),
                               std::make_move_iterator(new_event_list.end()));
  }

  d->probe_event_list_cv.notify_one();
}
} // namespace trailofbits