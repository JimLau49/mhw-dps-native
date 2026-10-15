#include "meter/fight_log_store.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

#include "log.h"

namespace dps::meter {
namespace {

using ojson = nlohmann::ordered_json;

constexpr int kSchemaVersion = 2;
constexpr wchar_t kIndexFile[] = L"index.json";

std::string Sanitize(const std::string& value) {
    std::string out;
    for (char c : value)
        out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return out.empty() ? "unknown" : out;
}

/// "2026-10-15T12:34:56.789Z", "...56.1234567+03:00" -> Unix ms (0 when unparsable).
int64_t ParseIso(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, consumed = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second, &consumed) < 6)
        return 0;
    size_t i = static_cast<size_t>(consumed);
    double fraction = 0.0;
    if (i < text.size() && text[i] == '.') {
        double scale = 0.1;
        for (++i; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i, scale *= 0.1)
            fraction += (text[i] - '0') * scale;
    }
    int offsetMinutes = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        int oh = 0, om = 0;
        if (std::sscanf(text.c_str() + i + 1, "%2d:%2d", &oh, &om) >= 1)
            offsetMinutes = (text[i] == '-' ? -1 : 1) * (oh * 60 + om);
    }

    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = static_cast<WORD>(second);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft))
        return 0;
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    const int64_t ms = (static_cast<int64_t>(value.QuadPart) - 116444736000000000LL) / 10000LL;
    return ms + static_cast<int64_t>(fraction * 1000.0) - static_cast<int64_t>(offsetMinutes) * 60'000LL;
}

template <typename T>
T Get(const nlohmann::json& json, const char* key, T fallback) {
    try {
        if (json.is_object() && json.contains(key) && !json[key].is_null())
            return json[key].get<T>();
    } catch (...) {
    }
    return fallback;
}

}  // namespace

FightLogStore& Logs() {
    static FightLogStore store;
    return store;
}

// ---- serialization ------------------------------------------------------------

std::string FightLogStore::Serialize(const FightLog& log) {
    ojson json;
    json["schemaVersion"] = kSchemaVersion;
    json["kind"] = log.kind;
    json["pluginVersion"] = std::string("native-") + kBuildStamp;
    json["gameBuild"] = log.gameBuild;
    json["questId"] = log.questId;
    json["questName"] = log.questName;
    json["result"] = log.result;
    json["stageId"] = log.stageId;
    json["stage"] = log.stage;
    json["startedAt"] = IsoUtc(log.startedAtMs);
    json["endedAt"] = IsoUtc(log.endedAtMs);
    json["durationSeconds"] = log.durationSeconds;
    json["timerSource"] = log.timerSource;
    json["hitCoverage"] = log.hitCoverage;
    if (log.rewards.present)
        json["rewards"] = {{"zenny", log.rewards.zenny}, {"hunterRankPoints", log.rewards.hunterRankPoints}, {"stars", log.rewards.stars}};

    ojson players = ojson::array();
    for (const auto& player : log.players) {
        ojson row;
        row["slot"] = player.slot;
        row["name"] = player.name;
        row["isLocal"] = player.isLocal;
        if (!player.weapon.empty())
            row["weapon"] = player.weapon;
        row["damage"] = player.damage;
        row["dps"] = player.dps;
        row["percent"] = player.percent;
        if (player.carts > 0)
            row["carts"] = player.carts;
        players.push_back(std::move(row));
    }
    json["players"] = std::move(players);

    ojson monsters = ojson::array();
    for (const auto& monster : log.monsters) {
        ojson row;
        row["id"] = monster.id;
        row["type"] = monster.type;
        row["name"] = monster.name;
        row["variant"] = monster.variant;
        row["maxHealth"] = monster.maxHealth;
        row["lastHealth"] = monster.lastHealth;
        row["firstSeenT"] = monster.firstSeenT;
        if (monster.diedT >= 0.0f)
            row["diedT"] = monster.diedT;
        monsters.push_back(std::move(row));
    }
    json["monsters"] = std::move(monsters);

    ojson samples = ojson::array();
    for (const auto& sample : log.samples)
        samples.push_back({{"t", sample.t}, {"damage", sample.damage}});
    json["samples"] = std::move(samples);

    ojson hits = ojson::array();
    for (const auto& hit : log.hits) {
        ojson row;
        row["t"] = hit.t;
        row["slot"] = hit.slot;
        if (!hit.monster.empty())
            row["monster"] = hit.monster;
        row["damage"] = hit.damage;
        row["crit"] = hit.crit;
        row["tenderized"] = hit.tenderized;
        row["attackId"] = hit.attackId;
        if (hit.part >= 0) {
            row["part"] = hit.part;
            if (!hit.partName.empty())
                row["partName"] = hit.partName;
        }
        row["actionSet"] = hit.actionSet;
        row["actionId"] = hit.actionId;
        if (!hit.action.empty())
            row["action"] = hit.action;
        hits.push_back(std::move(row));
    }
    json["hits"] = std::move(hits);

    ojson events = ojson::array();
    for (const auto& event : log.events) {
        ojson row;
        row["t"] = event.t;
        row["type"] = event.type;
        if (event.slot >= 0)
            row["slot"] = event.slot;
        if (!event.monster.empty())
            row["monster"] = event.monster;
        if (event.hasDetail)
            row["detail"] = event.detail;
        events.push_back(std::move(row));
    }
    json["events"] = std::move(events);

    // Replace invalid UTF-8 (a garbled hunter name) instead of throwing.
    return json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

std::string FightLogStore::FileNameFor(const FightLog& log) {
    const std::string stamp = LocalTimeString(log.startedAtMs, "%Y-%m-%d_%H%M%S");
    const std::string middle = log.kind == "trial" ? StrFormat("trial%.0fs", log.durationSeconds) : std::to_string(log.questId);
    return stamp + "_" + middle + "_" + Sanitize(log.result) + ".json";
}

bool FightLogStore::ParseLogFile(const std::wstring& path, FightLog& out) {
    std::string text;
    if (!ReadWholeFile(path, text))
        return false;
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object())
        return false;
    try {
        out = FightLog{};
        out.kind = Get<std::string>(json, "kind", "quest");
        out.questId = Get<int>(json, "questId", 0);
        out.questName = Get<std::string>(json, "questName", "");
        out.result = Get<std::string>(json, "result", "");
        out.startedAtMs = ParseIso(Get<std::string>(json, "startedAt", ""));
        out.durationSeconds = Get<float>(json, "durationSeconds", 0.0f);
        if (json.contains("players") && json["players"].is_array()) {
            for (const auto& row : json["players"]) {
                LogPlayer player;
                player.slot = Get<int>(row, "slot", 0);
                player.name = Get<std::string>(row, "name", "");
                player.isLocal = Get<bool>(row, "isLocal", false);
                player.weapon = Get<std::string>(row, "weapon", "");
                player.damage = Get<int>(row, "damage", 0);
                player.dps = Get<float>(row, "dps", 0.0f);
                player.percent = Get<float>(row, "percent", 0.0f);
                out.players.push_back(player);
            }
        }
        if (json.contains("monsters") && json["monsters"].is_array()) {
            for (const auto& row : json["monsters"]) {
                LogMonster monster;
                monster.name = Get<std::string>(row, "name", "");
                out.monsters.push_back(monster);
            }
        }
        out.hits.resize(json.contains("hits") && json["hits"].is_array() ? json["hits"].size() : 0);
        out.events.resize(json.contains("events") && json["events"].is_array() ? json["events"].size() : 0);
    } catch (...) {
        return false;
    }
    const size_t slash = path.find_last_of(L"\\/");
    out.fileName = WideToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
    return true;
}

FightLogStore::IndexEntry FightLogStore::EntryFrom(const FightLog& log) {
    IndexEntry entry;
    entry.file = log.fileName;
    entry.kind = log.kind;
    entry.questId = log.questId;
    entry.questName = log.questName;
    entry.result = log.result;
    entry.startedAtMs = log.startedAtMs;
    entry.startedAt = IsoUtc(log.startedAtMs);
    entry.durationSeconds = log.durationSeconds;
    std::set<std::string> seen;
    for (const auto& player : log.players) {
        entry.totalDamage += player.damage;
        entry.players.push_back(player.name);
        if (player.isLocal && !player.weapon.empty())
            entry.weapon = player.weapon;
    }
    for (const auto& monster : log.monsters) {
        if (seen.insert(monster.name).second)
            entry.monsters.push_back(monster.name);
    }
    return entry;
}

HistoryEntry FightLogStore::HistoryFrom(const FightLog& log) {
    HistoryEntry entry;
    entry.fileName = log.fileName;
    entry.kind = log.kind;
    entry.questName = log.questName;
    entry.result = log.result;
    entry.durationSeconds = log.durationSeconds;
    entry.startedAtMs = log.startedAtMs;
    entry.players = log.players;
    std::set<std::string> seen;
    for (const auto& monster : log.monsters) {
        if (seen.insert(monster.name).second)
            entry.monsters.push_back(monster.name);
    }
    entry.hits = log.hits.size();
    entry.events = log.events.size();
    return entry;
}

// ---- store ----------------------------------------------------------------------

void FightLogStore::Init(const std::wstring& dataDirectory) {
    LockGuard guard(lock_);
    directory_ = JoinPath(dataDirectory, L"logs");
    available_ = EnsureDirectory(directory_);
    if (!available_) {
        LOG_WARN("could not create %s; fight logs disabled", WideToUtf8(directory_).c_str());
        return;
    }

    std::string text;
    bool rebuild = true;
    if (ReadWholeFile(JoinPath(directory_, kIndexFile), text)) {
        const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
        if (json.is_array()) {
            rebuild = false;
            for (const auto& row : json) {
                IndexEntry entry;
                entry.file = Get<std::string>(row, "file", "");
                entry.kind = Get<std::string>(row, "kind", "quest");
                entry.questId = Get<int>(row, "questId", 0);
                entry.questName = Get<std::string>(row, "questName", "");
                entry.result = Get<std::string>(row, "result", "");
                entry.startedAt = Get<std::string>(row, "startedAt", "");
                entry.startedAtMs = ParseIso(entry.startedAt);
                entry.durationSeconds = Get<float>(row, "durationSeconds", 0.0f);
                entry.totalDamage = Get<int>(row, "totalDamage", 0);
                entry.weapon = Get<std::string>(row, "weapon", "");
                entry.players = Get<std::vector<std::string>>(row, "players", {});
                entry.monsters = Get<std::vector<std::string>>(row, "monsters", {});
                // A logged hunt never has 0 damage: such an entry predates totalDamage.
                if (entry.file.empty() || entry.totalDamage == 0)
                    rebuild = true;
                index_.push_back(std::move(entry));
            }
        }
    }
    if (rebuild)
        RebuildIndexLocked();

    // Recent history for the F9 panel: the newest log files.
    auto files = LogFiles();
    std::vector<std::pair<FILETIME, std::wstring>> dated;
    for (const auto& file : files) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attributes))
            dated.emplace_back(attributes.ftLastWriteTime, file);
    }
    std::sort(dated.begin(), dated.end(), [](const auto& a, const auto& b) { return CompareFileTime(&a.first, &b.first) > 0; });
    for (size_t i = 0; i < dated.size() && history_.size() < kHistoryLimit; ++i) {
        FightLog log;
        if (ParseLogFile(dated[i].second, log))
            history_.push_back(HistoryFrom(log));
    }
    LOG_INFO("fight logs: %s (%zu indexed, %zu in history)", WideToUtf8(directory_).c_str(), index_.size(), history_.size());
}

bool FightLogStore::Available() {
    LockGuard guard(lock_);
    return available_;
}

std::vector<std::wstring> FightLogStore::LogFiles() const {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(JoinPath(directory_, L"*_*_*.json").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return files;
    do {
        const std::wstring name = data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsnicmp(name.c_str(), L"live-debug", 10) == 0)
            continue;
        files.push_back(JoinPath(directory_, name));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return files;
}

void FightLogStore::RebuildIndexLocked() {
    index_.clear();
    for (const auto& file : LogFiles()) {
        FightLog log;
        if (ParseLogFile(file, log))
            index_.push_back(EntryFrom(log));
    }
    std::sort(index_.begin(), index_.end(), [](const IndexEntry& a, const IndexEntry& b) { return a.startedAtMs > b.startedAtMs; });
    if (!index_.empty()) {
        WriteIndexLocked();
        LOG_INFO("rebuilt index.json from %zu fight logs", index_.size());
    }
}

void FightLogStore::WriteIndexLocked() {
    ojson json = ojson::array();
    for (const auto& entry : index_) {
        ojson row;
        row["file"] = entry.file;
        row["kind"] = entry.kind;
        row["questId"] = entry.questId;
        row["questName"] = entry.questName;
        row["result"] = entry.result;
        row["startedAt"] = entry.startedAt.empty() ? IsoUtc(entry.startedAtMs) : entry.startedAt;
        row["durationSeconds"] = entry.durationSeconds;
        row["totalDamage"] = entry.totalDamage;
        if (!entry.weapon.empty())
            row["weapon"] = entry.weapon;
        row["players"] = entry.players;
        row["monsters"] = entry.monsters;
        json.push_back(std::move(row));
    }
    if (!WriteFileAtomic(JoinPath(directory_, kIndexFile), json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n"))
        LOG_WARN("could not write index.json");
}

void FightLogStore::AddToIndexLocked(const FightLog& log) {
    index_.erase(std::remove_if(index_.begin(), index_.end(), [&](const IndexEntry& e) { return e.file == log.fileName; }),
                 index_.end());
    index_.insert(index_.begin(), EntryFrom(log));
    WriteIndexLocked();

    history_.insert(history_.begin(), HistoryFrom(log));
    if (history_.size() > kHistoryLimit)
        history_.resize(kHistoryLimit);
}

bool FightLogStore::Write(FightLog& log) {
    std::wstring directory;
    {
        LockGuard guard(lock_);
        if (!available_)
            return false;
        directory = directory_;
    }
    log.fileName = FileNameFor(log);
    const std::string text = Serialize(log);
    if (!WriteFileAtomic(JoinPath(directory, Utf8ToWide(log.fileName)), text)) {
        LOG_WARN("could not write fight log %s; further logs disabled this session", log.fileName.c_str());
        LockGuard guard(lock_);
        available_ = false;
        return false;
    }
    LOG_INFO("wrote fight log %s (%zu hits, %zu events, %zu bytes)", log.fileName.c_str(), log.hits.size(), log.events.size(),
             text.size());
    LockGuard guard(lock_);
    AddToIndexLocked(log);
    return true;
}

namespace {

struct SaveJob {
    FightLogStore* store;
    FightLog log;
    std::function<void(const std::string&)> onSaved;
};

}  // namespace

void FightLogStore::SaveAsync(FightLog log, std::function<void(const std::string&)> onSaved) {
    auto* job = new SaveJob{this, std::move(log), std::move(onSaved)};
    const bool started = RunDetached(
        [](void* arg) {
            std::unique_ptr<SaveJob> job(static_cast<SaveJob*>(arg));
            try {
                if (job->store->Write(job->log) && job->onSaved)
                    job->onSaved(job->log.fileName);
            } catch (const std::exception& error) {
                LOG_ERROR("fight log write failed: %s", error.what());
            } catch (...) {
                LOG_ERROR("fight log write failed");
            }
        },
        job);
    if (!started) {
        std::unique_ptr<SaveJob> owned(job);
        if (Write(owned->log) && owned->onSaved)
            owned->onSaved(owned->log.fileName);
    }
}

void FightLogStore::BeginHunt() {
    samples_.clear();
    samples_.push_back(LogSample{});
}

void FightLogStore::UpdateSamples(float elapsed, const std::array<int, kPartySlots>& slotDamage) {
    while (samples_.size() < kMaxSamples) {
        const float nextT = static_cast<float>(samples_.size()) * kSampleInterval;
        if (elapsed + 0.05f < nextT)
            return;
        LogSample sample;
        sample.t = nextT;
        sample.damage = slotDamage;
        samples_.push_back(sample);
    }
}

std::vector<LogSample> FightLogStore::TakeSamples() {
    std::vector<LogSample> out;
    out.swap(samples_);
    return out;
}

std::optional<int> FightLogStore::BestTrial(const std::string& weapon, int durationSeconds) {
    LockGuard guard(lock_);
    std::optional<int> best;
    for (const auto& entry : index_) {
        if (entry.kind != "trial" || entry.weapon != weapon ||
            std::fabs(entry.durationSeconds - static_cast<float>(durationSeconds)) >= 0.5f)
            continue;
        if (!best || entry.totalDamage > *best)
            best = entry.totalDamage;
    }
    return best;
}

std::vector<BestTrialEntry> FightLogStore::Bests() {
    LockGuard guard(lock_);
    std::map<std::pair<std::string, long>, BestTrialEntry> best;
    for (const auto& entry : index_) {
        if (entry.kind != "trial")
            continue;
        const auto key = std::make_pair(entry.weapon.empty() ? std::string("?") : entry.weapon, std::lround(entry.durationSeconds));
        auto it = best.find(key);
        if (it == best.end() || entry.totalDamage > it->second.totalDamage)
            best[key] = BestTrialEntry{key.first, entry.durationSeconds, entry.totalDamage, entry.startedAtMs};
    }
    std::vector<BestTrialEntry> out;
    for (auto& [key, entry] : best)
        out.push_back(entry);
    return out;
}

std::vector<HistoryEntry> FightLogStore::History() {
    LockGuard guard(lock_);
    return history_;
}

}  // namespace dps::meter
