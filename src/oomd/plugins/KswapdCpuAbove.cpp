#include "oomd/plugins/KswapdCpuAbove.h"

#include <algorithm>
#include <dirent.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>

#include "oomd/Log.h"
#include "oomd/PluginRegistry.h"
#include "oomd/util/Fs.h"
#include "oomd/util/PluginArgParser.h"

namespace Oomd {

REGISTER_PLUGIN(kswapd_cpu_above, KswapdCpuAbove::create);

int KswapdCpuAbove::init(
    const Engine::PluginArgs& args,
    const PluginConstructionContext& context) {
  argParser_.addArgument("threshold", threshold_, true);
  argParser_.addArgument("duration", duration_, true);
  
  if (!argParser_.parse(args)) {
    return 1;
  }
  
  // Initialize last check time
  last_check_time_ = std::chrono::steady_clock::now();
  
  // Success
  return 0;
}

SystemMaybe<std::vector<int>> KswapdCpuAbove::findKswapdPids() {
  DIR* dir = opendir("/proc");
  if (!dir) {
    return SYSTEM_ERROR(std::make_error_code(std::errc::io_error), "Failed to open /proc");
  }
  
  std::vector<int> kswapd_pids;
  struct dirent* entry;
  
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    
    // Skip non-numeric entries
    bool is_numeric = true;
    for (char c : name) {
      if (!isdigit(c)) {
        is_numeric = false;
        break;
      }
    }
    
    if (!is_numeric) {
      continue;
    }
    
    // Read /proc/[pid]/comm to check if it's kswapd
    std::string comm_path = "/proc/" + name + "/comm";
    auto comm_maybe = Fs::readFileByLine(comm_path);
    
    if (comm_maybe && !comm_maybe->empty()) {
      std::string comm = comm_maybe->at(0);
      // Remove newline if present
      if (!comm.empty() && comm.back() == '\n') {
        comm.pop_back();
      }
      
      // Check if process name starts with "kswapd" (kswapd0, kswapd1, etc.)
      if (comm.find("kswapd") == 0) {
        kswapd_pids.push_back(std::stoi(name));
      }
    }
  }
  
  closedir(dir);
  
  if (kswapd_pids.empty()) {
    return SYSTEM_ERROR(std::make_error_code(std::errc::no_such_process), "kswapd processes not found");
  }
  
  return kswapd_pids;
}

bool KswapdCpuAbove::isKswapdPid(int pid) {
  std::string comm_path = "/proc/" + std::to_string(pid) + "/comm";
  auto comm_maybe = Fs::readFileByLine(comm_path);

  if (!comm_maybe || comm_maybe->empty()) {
    return false;
  }

  std::string comm = comm_maybe->at(0);
  if (!comm.empty() && comm.back() == '\n') {
    comm.pop_back();
  }

  return comm.find("kswapd") == 0;
}

SystemMaybe<std::vector<int>> KswapdCpuAbove::getKswapdPids() {
  if (!cached_kswapd_pids_.empty()) {
    // Validate cached PIDs — check each is still a kswapd process
    bool all_valid = true;
    for (int pid : cached_kswapd_pids_) {
      if (!isKswapdPid(pid)) {
        all_valid = false;
        break;
      }
    }

    if (all_valid) {
      return cached_kswapd_pids_;
    }

    // Some cached PID is stale — clear cache and rescan
    cached_kswapd_pids_.clear();
    last_cpu_stats_.clear();
  }

  // Full scan of /proc
  auto pids_maybe = findKswapdPids();
  if (pids_maybe) {
    cached_kswapd_pids_ = *pids_maybe;
  }
  return pids_maybe;
}

SystemMaybe<KswapdCpuAbove::CpuStats> KswapdCpuAbove::readCpuStats(int pid) {
  std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
  auto lines_maybe = Fs::readFileByLine(stat_path);
  
  if (!lines_maybe || lines_maybe->empty()) {
    return SYSTEM_ERROR(std::make_error_code(std::errc::io_error), "Failed to read " + stat_path);
  }
  
  std::string line = lines_maybe->at(0);
  std::istringstream iss(line);
  
  CpuStats stats;
  std::string pid_str;
  std::string comm;
  
  // Parse /proc/[pid]/stat format
  // pid (comm) state ... utime(14) stime(15) ...
  iss >> pid_str;
  
  // Read comm (in parentheses, may contain spaces)
  std::getline(iss, comm, '(');
  std::getline(iss, comm, ')');
  
  char state;
  iss >> state;
  
  // Skip fields 4-13 (ppid, pgrp, session, tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt)
  for (int i = 0; i < 11; i++) {
    std::string dummy;
    iss >> dummy;
  }
  
  // Field 14: utime
  iss >> stats.utime;
  // Field 15: stime
  iss >> stats.stime;
  
  stats.total_time = stats.utime + stats.stime;
  
  if (iss.fail()) {
    return SYSTEM_ERROR(std::make_error_code(std::errc::io_error), "Failed to parse CPU stats");
  }
  
  return stats;
}

double KswapdCpuAbove::calculateCpuUsage(
    const CpuStats& current,
    const CpuStats& previous,
    std::chrono::milliseconds elapsed) {
  if (elapsed.count() == 0) {
    return 0.0;
  }
  
  // Get system clock ticks per second
  long clk_tck = sysconf(_SC_CLK_TCK);
  
  // Calculate CPU time used in seconds
  double cpu_time_used = static_cast<double>(current.total_time - previous.total_time) / clk_tck;
  
  // Calculate elapsed time in seconds
  double elapsed_seconds = elapsed.count() / 1000.0;
  
  // Calculate CPU usage percentage
  double cpu_usage = (cpu_time_used / elapsed_seconds) * 100.0;
  
  return cpu_usage;
}

Engine::PluginRet KswapdCpuAbove::run(OomdContext& /* ctx */) {
  using clock = std::chrono::steady_clock;
  
  // Get kswapd PIDs (uses cache when possible)
  auto pids_maybe = getKswapdPids();
  if (!pids_maybe) {
    OLOG << "Failed to find kswapd PIDs: " << pids_maybe.error().what();
    return Engine::PluginRet::STOP;
  }
  
  auto now = clock::now();
  double total_cpu_usage = 0.0;

  // Calculate CPU usage for each kswapd process
  for (int pid : *pids_maybe) {
    // Read current CPU stats
    auto current_stats_maybe = readCpuStats(pid);
    if (!current_stats_maybe) {
      OLOG << "Failed to read CPU stats for PID " << pid << ": " << current_stats_maybe.error().what();
      continue;
    }
    
    double cpu_usage = 0.0;
    
    // Calculate CPU usage if we have previous stats
    auto it = last_cpu_stats_.find(pid);
    if (it != last_cpu_stats_.end()) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check_time_);
      cpu_usage = calculateCpuUsage(*current_stats_maybe, it->second, elapsed);
    }
    
    // Store current stats for next iteration
    last_cpu_stats_[pid] = *current_stats_maybe;
    
    // Sum CPU usage across all kswapd processes
    total_cpu_usage += cpu_usage;
  }

  last_check_time_ = now;

  // Update sliding window: add new sample, remove oldest if window is full
  cpu_usage_history_.push_back(total_cpu_usage);
  cpu_usage_sum_ += total_cpu_usage;
  while (static_cast<int>(cpu_usage_history_.size()) > duration_) {
    cpu_usage_sum_ -= cpu_usage_history_.front();
    cpu_usage_history_.pop_front();
  }

  // Check average CPU usage over the sliding window
  if (static_cast<int>(cpu_usage_history_.size()) == duration_) {
    double avg_cpu_usage = cpu_usage_sum_ / duration_;

    // Log current CPU usage if non-zero
    if (avg_cpu_usage > 0.0) {
      OLOG << "kswapd avg CPU usage over " << duration_ << " seconds: " << std::fixed << std::setprecision(2)
          << avg_cpu_usage << "%";
    }

    if (avg_cpu_usage > threshold_) {
      std::ostringstream oss;
      oss << std::setprecision(2) << std::fixed;
      oss << "kswapd average CPU usage " << avg_cpu_usage
          << "% is over the threshold of " << threshold_
          << "% (averaged over " << duration_ << " iterations)";
      OLOG << oss.str();

      return Engine::PluginRet::CONTINUE;
    }
  }

  return Engine::PluginRet::STOP;
}

} // namespace Oomd
