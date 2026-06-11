/*
 * Copyright (C) 2018-present, Facebook, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#include <chrono>
#include <deque>
#include <string>

#include "oomd/engine/BasePlugin.h"
#include "oomd/OomdContext.h"
#include "oomd/PluginConstructionContext.h"
#include "oomd/util/SystemMaybe.h"

namespace Oomd {

class DiskBusyAbove : public Engine::BasePlugin {
 public:
  int init(
      const Engine::PluginArgs& args,
      const PluginConstructionContext& context) override;

  Engine::PluginRet run(OomdContext& /* unused */) override;

  static DiskBusyAbove* create() {
    return new DiskBusyAbove();
  }

  ~DiskBusyAbove() = default;

 private:
  struct DiskStats {
    uint64_t ioTicks; // field 13: time doing I/Os (ms)
  };

  // Read disk stats from /proc/diskstats for the given disk name
  SystemMaybe<DiskStats> readDiskStats(const std::string& diskName);

  // Calculate busy percentage
  double calculateBusy(
      const DiskStats& current,
      const DiskStats& previous,
      std::chrono::milliseconds elapsed);

  std::string diskName_; // Disk name to monitor (e.g., "sda")
  int threshold_;        // Busy threshold in percentage
  int duration_;         // Duration in seconds (number of samples) to check

  DiskStats lastStats_;
  std::chrono::steady_clock::time_point lastCheckTime_;
  bool hasPreviousStats_{false};

  // Sliding window of busy samples and running sum for O(1) average
  std::deque<double> busyHistory_;
  double busySum_{0.0};
};

} // namespace Oomd
