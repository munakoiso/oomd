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

#include "oomd/plugins/DiskBusyAbove.h"

#include <iomanip>
#include <sstream>
#include <string>

#include "oomd/Log.h"
#include "oomd/PluginRegistry.h"
#include "oomd/util/Fs.h"
#include "oomd/util/PluginArgParser.h"

namespace Oomd {

REGISTER_PLUGIN(disk_busy_above, DiskBusyAbove::create);

int DiskBusyAbove::init(
    const Engine::PluginArgs& args,
    const PluginConstructionContext& context) {
  argParser_.addArgument("disk", diskName_, true);
  argParser_.addArgument("threshold", threshold_, true);
  argParser_.addArgument("duration", duration_, true);

  if (!argParser_.parse(args)) {
    return 1;
  }

  // Initialize last check time
  lastCheckTime_ = std::chrono::steady_clock::now();

  return 0;
}

SystemMaybe<DiskBusyAbove::DiskStats>
DiskBusyAbove::readDiskStats(const std::string& diskName) {
  auto lines_maybe = Fs::readFileByLine("/proc/diskstats");
  if (!lines_maybe || lines_maybe->empty()) {
    return SYSTEM_ERROR(
        std::make_error_code(std::errc::io_error),
        "Failed to read /proc/diskstats");
  }

  for (const auto& line : *lines_maybe) {
    std::istringstream iss(line);

    unsigned int major, minor;
    std::string name;

    iss >> major >> minor >> name;
    if (iss.fail()) {
      continue;
    }

    if (name != diskName) {
      continue;
    }

    // /proc/diskstats fields (kernel 5.5+):
    //  1: major
    //  2: minor
    //  3: name
    //  4: rd_ios
    //  5: rd_merges
    //  6: rd_sectors
    //  7: rd_ms
    //  8: wr_ios
    //  9: wr_merges
    // 10: wr_sectors
    // 11: wr_ms
    // 12: in_progress
    // 13: io_ticks (ms) — this is what atop uses for busy%
    // 14: time_in_queue (ms)
    //
    // Skip fields 4-12 (9 fields: rd_ios, rd_merges, rd_sectors, rd_ms,
    //                    wr_ios, wr_merges, wr_sectors, wr_ms, in_progress)
    uint64_t dummy;
    for (int i = 0; i < 9; i++) {
      iss >> dummy;
    }

    DiskStats stats;
    iss >> stats.ioTicks;

    if (iss.fail()) {
      return SYSTEM_ERROR(
          std::make_error_code(std::errc::io_error),
          "Failed to parse io_ticks for disk " + diskName);
    }

    return stats;
  }

  return SYSTEM_ERROR(
      std::make_error_code(std::errc::no_such_device),
      "Disk " + diskName + " not found in /proc/diskstats");
}

double DiskBusyAbove::calculateBusy(
    const DiskStats& current,
    const DiskStats& previous,
    std::chrono::milliseconds elapsed) {
  if (elapsed.count() == 0) {
    return 0.0;
  }

  // io_ticks is in milliseconds
  double deltaIoTicks = static_cast<double>(current.ioTicks - previous.ioTicks);
  double elapsedMs = static_cast<double>(elapsed.count());

  // busy% = (delta_io_ticks / delta_elapsed) * 100
  double busy = (deltaIoTicks / elapsedMs) * 100.0;

  // Cap at 100% (io_ticks can sometimes exceed elapsed due to accounting)
  if (busy > 100.0) {
    busy = 100.0;
  }
  if (busy < 0.0) {
    busy = 0.0;
  }

  return busy;
}

Engine::PluginRet DiskBusyAbove::run(OomdContext& /* ctx */) {
  using clock = std::chrono::steady_clock;

  auto now = clock::now();

  // Read current disk stats
  auto currentStatsMaybe = readDiskStats(diskName_);
  if (!currentStatsMaybe) {
    OLOG << "Failed to read disk stats for " << diskName_ << ": "
         << currentStatsMaybe.error().what();
    return Engine::PluginRet::STOP;
  }

  double busy = 0.0;

  if (hasPreviousStats_) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastCheckTime_);
    busy = calculateBusy(*currentStatsMaybe, lastStats_, elapsed);
  }

  // Store current stats for next iteration
  lastStats_ = *currentStatsMaybe;
  lastCheckTime_ = now;
  hasPreviousStats_ = true;

  // Update sliding window: add new sample, remove oldest if window is full
  busyHistory_.push_back(busy);
  busySum_ += busy;
  while (static_cast<int>(busyHistory_.size()) > duration_) {
    busySum_ -= busyHistory_.front();
    busyHistory_.pop_front();
  }

  // Check average busy over the sliding window
  if (static_cast<int>(busyHistory_.size()) == duration_) {
    double avgBusy = busySum_ / duration_;

    if (avgBusy > 0.0) {
      OLOG << "Disk " << diskName_ << " avg busy over " << duration_
           << " seconds: " << std::fixed << std::setprecision(2) << avgBusy
           << "%";
    }

    if (avgBusy > threshold_) {
      std::ostringstream oss;
      oss << std::setprecision(2) << std::fixed;
      oss << "Disk " << diskName_ << " average busy " << avgBusy
          << "% is over the threshold of " << threshold_
          << "% (averaged over " << duration_ << " iterations)";
      OLOG << oss.str();

      return Engine::PluginRet::CONTINUE;
    }
  }

  return Engine::PluginRet::STOP;
}

} // namespace Oomd
