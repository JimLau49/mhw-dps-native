#include "address_map.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>

#include "common.h"
#include "log.h"

// The shipped maps are compiled in verbatim (paths relative to the project root,
// where make runs the compiler), so the DLL works without any data files.
__asm__(
    ".section .rdata,\"dr\"\n"
    "dps_map_421810:\n"
    ".incbin \"addresses/MonsterHunterWorld.421810.map\"\n"
    ".byte 0\n"
    "dps_map_421631:\n"
    ".incbin \"addresses/MonsterHunterWorld.421631.map\"\n"
    ".byte 0\n"
    ".text\n");

extern "C" const char dps_map_421810[];
extern "C" const char dps_map_421631[];

namespace dps {
namespace {

struct EmbeddedMap {
    int build;
    const char* text;
};

const EmbeddedMap kEmbedded[] = {
    {421810, dps_map_421810},
    {421631, dps_map_421631},
};

bool ParseNumber(std::string_view token, long long& out) {
    std::string text(token);
    char* end = nullptr;
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    errno = 0;
    out = std::strtoll(hex ? text.c_str() + 2 : text.c_str(), &end, hex ? 16 : 10);
    return errno == 0 && end && *end == '\0' && !text.empty();
}

std::vector<std::string_view> Tokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t')
            ++j;
        if (j > i)
            tokens.push_back(line.substr(i, j - i));
        i = j;
    }
    return tokens;
}

}  // namespace

std::vector<int> AddressMap::EmbeddedBuilds() {
    std::vector<int> builds;
    for (const auto& map : kEmbedded)
        builds.push_back(map.build);
    return builds;
}

void AddressMap::Parse(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;

        const size_t comment = line.find('#');
        if (comment != std::string_view::npos)
            line = line.substr(0, comment);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);

        const auto tokens = Tokens(line);
        if (tokens.size() < 3)
            continue;

        if (tokens[0] == "Address") {
            long long value = 0;
            if (ParseNumber(tokens[2], value) && value >= 0)
                addresses_[std::string(tokens[1])] = static_cast<uintptr_t>(value);
        } else if (tokens[0] == "Offset") {
            std::vector<int> values;
            bool ok = true;
            std::string_view list = tokens[2];
            size_t i = 0;
            while (ok && i <= list.size()) {
                size_t comma = list.find(',', i);
                if (comma == std::string_view::npos)
                    comma = list.size();
                long long value = 0;
                ok = ParseNumber(list.substr(i, comma - i), value) && value >= INT32_MIN && value <= INT32_MAX;
                if (ok)
                    values.push_back(static_cast<int>(value));
                i = comma + 1;
            }
            if (ok && !values.empty())
                offsets_[std::string(tokens[1])] = values;
        }
    }
}

AddressMap AddressMap::Load(int build) {
    AddressMap map;
    map.build = build;
    for (const auto& embedded : kEmbedded) {
        if (embedded.build == build) {
            map.Parse(embedded.text);
            map.embedded = true;
            map.source = StrFormat("built-in %d", build);
        }
    }

    const std::wstring file =
        JoinPath(JoinPath(DataDirectory(), L"Addresses"), Utf8ToWide(StrFormat("MonsterHunterWorld.%d.map", build)));
    std::string text;
    if (ReadWholeFile(file, text)) {
        const size_t before = map.addresses_.size() + map.offsets_.size();
        map.Parse(text);
        map.source = map.source.empty() ? WideToUtf8(file) : map.source + " + " + WideToUtf8(file);
        LOG_INFO("address map override %s (%zu keys before merge)", WideToUtf8(file).c_str(), before);
    }
    return map;
}

bool AddressMap::Address(const std::string& key, uintptr_t& rva) const {
    auto it = addresses_.find(key);
    if (it == addresses_.end() || it->second == 0)
        return false;
    rva = it->second;
    return true;
}

bool AddressMap::Offsets(const std::string& key, std::vector<int>& offsets) const {
    auto it = offsets_.find(key);
    if (it == offsets_.end())
        return false;
    offsets = it->second;
    return true;
}

uintptr_t AddressMap::AddressOr(const std::string& key, uintptr_t fallback) const {
    uintptr_t value = 0;
    return Address(key, value) ? value : fallback;
}

std::vector<int> AddressMap::OffsetsOr(const std::string& key) const {
    std::vector<int> value;
    Offsets(key, value);
    return value;
}

}  // namespace dps
