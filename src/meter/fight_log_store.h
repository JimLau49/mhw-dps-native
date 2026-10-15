#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "meter/recorder.h"

namespace dps::meter {

struct HistoryEntry {
    std::string fileName;
    std::string kind;
    std::string questName;
    std::string result;
    float durationSeconds = 0.0f;
    int64_t startedAtMs = 0;
    std::vector<LogPlayer> players;
    std::vector<std::string> monsters;
    size_t hits = 0;
    size_t events = 0;
};

struct BestTrialEntry {
    std::string weapon;
    float durationSeconds = 0.0f;
    int totalDamage = 0;
    int64_t startedAtMs = 0;
};

/// Writes one JSON file per hunt to logs/, keeps logs/index.json (newest first) for
/// the viewers, and holds the recent history shown in the F9 panel.
class FightLogStore {
public:
    static constexpr size_t kMaxSamples = 900;
    static constexpr float kSampleInterval = 2.0f;
    static constexpr size_t kHistoryLimit = 20;

    /// Loads index.json (rebuilding it from the log files when missing or stale) and
    /// the recent history. Reads files: call off the game thread.
    void Init(const std::wstring& dataDirectory);
    bool Available();
    std::wstring Directory() const { return directory_; }

    // Update thread.
    void BeginHunt();
    /// One sample per kSampleInterval of hunt time; repeats when the clock jumps.
    void UpdateSamples(float elapsed, const std::array<int, kPartySlots>& slotDamage);
    std::vector<LogSample> TakeSamples();

    /// Serializes and writes on a background thread; index and history update when
    /// done, then onSaved(fileName) runs on that thread.
    void SaveAsync(FightLog log, std::function<void(const std::string&)> onSaved = {});

    /// Highest saved trial damage for this weapon and duration.
    std::optional<int> BestTrial(const std::string& weapon, int durationSeconds);
    std::vector<BestTrialEntry> Bests();
    std::vector<HistoryEntry> History();

    static std::string Serialize(const FightLog& log);
    static std::string FileNameFor(const FightLog& log);

private:
    struct IndexEntry {
        std::string file;
        std::string kind = "quest";
        int questId = 0;
        std::string questName;
        std::string result;
        std::string startedAt;  // as written (ISO 8601)
        int64_t startedAtMs = 0;
        float durationSeconds = 0.0f;
        int totalDamage = 0;
        std::string weapon;
        std::vector<std::string> players;
        std::vector<std::string> monsters;
    };

    bool Write(FightLog& log);
    void AddToIndexLocked(const FightLog& log);
    void WriteIndexLocked();
    void RebuildIndexLocked();
    static IndexEntry EntryFrom(const FightLog& log);
    static HistoryEntry HistoryFrom(const FightLog& log);
    static bool ParseLogFile(const std::wstring& path, FightLog& out);
    std::vector<std::wstring> LogFiles() const;

    Lock lock_;
    std::wstring directory_;
    bool available_ = false;
    std::vector<IndexEntry> index_;
    std::vector<HistoryEntry> history_;
    std::vector<LogSample> samples_;  // update thread only
};

FightLogStore& Logs();

}  // namespace dps::meter
