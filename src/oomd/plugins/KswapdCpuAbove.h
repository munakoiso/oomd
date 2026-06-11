#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>

#include "oomd/engine/BasePlugin.h"
#include "oomd/OomdContext.h"
#include "oomd/PluginConstructionContext.h"
#include "oomd/util/SystemMaybe.h"

namespace Oomd {

class KswapdCpuAbove : public Engine::BasePlugin {
 public:
  int init(
      const Engine::PluginArgs& args,
      const PluginConstructionContext& context) override;

  Engine::PluginRet run(OomdContext& /* unused */) override;

  static KswapdCpuAbove* create() {
    return new KswapdCpuAbove();
  }

  ~KswapdCpuAbove() = default;

 private:
  struct CpuStats {
    uint64_t utime;
    uint64_t stime;
    uint64_t total_time;
  };

  // Find kswapd process PIDs by scanning /proc
  SystemMaybe<std::vector<int>> findKswapdPids();

  // Get kswapd PIDs, using cache when possible.
  // Validates cached PIDs and rescans /proc only if needed.
  SystemMaybe<std::vector<int>> getKswapdPids();

  // Check if a given PID is still a kswapd process
  bool isKswapdPid(int pid);

  // Read CPU stats from /proc/[pid]/stat
  SystemMaybe<CpuStats> readCpuStats(int pid);

  // Calculate CPU usage percentage
  double calculateCpuUsage(
      const CpuStats& current,
      const CpuStats& previous,
      std::chrono::milliseconds elapsed);

  int threshold_; // CPU usage threshold in percentage
  int duration_; // Duration in seconds to check

  std::vector<int> cached_kswapd_pids_;
  std::unordered_map<int, CpuStats> last_cpu_stats_;
  std::chrono::steady_clock::time_point last_check_time_;

  // Sliding window of CPU usage samples and running sum for O(1) average
  std::deque<double> cpu_usage_history_;
  double cpu_usage_sum_{0.0};
};

} // namespace Oomd
