#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dps {

/// HunterPie-format address map: "Address KEY 0xRVA" and "Offset KEY 0x1,0x2".
class AddressMap {
public:
    /// Built-in map for build, merged with nativePC/plugins/MhwDpsMeter/Addresses/
    /// MonsterHunterWorld.<build>.map when that file exists (file values win).
    static AddressMap Load(int build);
    /// Builds for which a map is compiled in.
    static std::vector<int> EmbeddedBuilds();

    void Parse(std::string_view text);

    bool Empty() const { return addresses_.empty(); }
    bool Address(const std::string& key, uintptr_t& rva) const;
    bool Offsets(const std::string& key, std::vector<int>& offsets) const;
    uintptr_t AddressOr(const std::string& key, uintptr_t fallback = 0) const;
    std::vector<int> OffsetsOr(const std::string& key) const;

    int build = 0;
    bool embedded = false;   // a compiled-in map exists for this build
    std::string source;      // "built-in", "built-in + <file>", ...

private:
    std::map<std::string, uintptr_t> addresses_;
    std::map<std::string, std::vector<int>> offsets_;
};

}  // namespace dps
