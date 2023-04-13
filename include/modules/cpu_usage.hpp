#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <fstream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "ALabel.hpp"
#include "util/sleeper_thread.hpp"

namespace waybar::modules {

class CpuUsage : public ALabel {
 public:
  CpuUsage(const std::string&, const Json::Value&);
  ~CpuUsage() = default;
  auto update() -> void;

 private:
  std::tuple<std::vector<uint16_t>, std::string> getCpuUsage();
  std::vector<std::tuple<size_t, size_t>> parseCpuinfo();

  std::vector<std::tuple<size_t, size_t>> prev_times_;

  util::SleeperThread thread_;
};

}  // namespace waybar::modules
