#pragma once

#include <string>
#include <vector>

namespace dps::log {

enum class Level { Debug, Info, Warn, Error };

/// Opens <path> for writing (the previous run is kept as <path>.prev).
void Init(const std::wstring& path);
void SetMinLevel(Level level);
void Write(Level level, const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
/// Most recent lines, oldest first (for the F9 panel).
std::vector<std::string> Recent(size_t count);
std::wstring Path();

}  // namespace dps::log

#define LOG_DEBUG(...) ::dps::log::Write(::dps::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::dps::log::Write(::dps::log::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) ::dps::log::Write(::dps::log::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::dps::log::Write(::dps::log::Level::Error, __VA_ARGS__)
