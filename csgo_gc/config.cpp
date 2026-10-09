#include "stdafx.h"
#include "config.h"
#include "keyvalue.h"
#include "random.h"

constexpr const char *ConfigFilePath = "csgo_gc/config.txt";

void GCConfig::Parse(const KeyValue& config)
{
    m_appIdOverride = config.GetNumber("appid_override", m_appIdOverride);
    m_showCsgoGCServersOnly = config.GetNumber("show_csgo_gc_servers_only", m_showCsgoGCServersOnly);

    const KeyValue *ranks = config.GetSubkey("ranks");
    if (ranks)
    {
        m_competitiveRank = ranks->GetNumber("competitive_rank", m_competitiveRank);
        m_competitiveWins = ranks->GetNumber("competitive_wins", m_competitiveWins);
        m_wingmanRank = ranks->GetNumber("wingman_rank", m_wingmanRank);
        m_wingmanWins = ranks->GetNumber("wingman_wins", m_wingmanWins);
        m_dangerZoneRank = ranks->GetNumber("dangerzone_rank", m_dangerZoneRank);
        m_dangerZoneWins = ranks->GetNumber("dangerzone_wins", m_dangerZoneWins);
    }

    m_forceMaxRarity = config.GetNumber("force_max_rarity", m_forceMaxRarity);
    m_destroyUsedItems = config.GetNumber("destroy_used_items", m_destroyUsedItems);
    m_randomizeFloat = config.GetNumber("randomize_item_float", m_randomizeFloat);

    const KeyValue *rarityWeights = config.GetSubkey("rarity_weights");
    if (rarityWeights)
    {
        m_rarityWeights.clear();
        m_rarityWeights.reserve(rarityWeights->SubkeyCount());
        for (const KeyValue &subkey : *rarityWeights)
        {
            RarityWeight weight;
            weight.rarity = FromString<uint32_t>(subkey.Name());
            weight.weight = FromString<float>(subkey.String());
            m_rarityWeights.push_back(weight);
        }
    }

    const KeyValue *friends = config.GetSubkey("friends");
    if (friends)
    {
        m_friends.clear();
        m_friends.reserve(friends->SubkeyCount());
        for (const KeyValue &subkey : *friends)
        {
            uint32_t friendId = FromString<uint32_t>(subkey.Name());
            m_friends.push_back(friendId);
        }
    }

    m_vacBanned = config.GetNumber("vac_banned", m_vacBanned);
    m_hasPrime = config.GetNumber("has_prime", 1);
    m_competitiveCooldownSeconds = config.GetNumber("competitive_cooldown_seconds", m_competitiveCooldownSeconds);
    m_commendedFriendly = config.GetNumber("cmd_friendly", m_commendedFriendly);
    m_commendedTeaching = config.GetNumber("cmd_teaching", m_commendedTeaching);
    m_commendedLeader = config.GetNumber("cmd_leader", m_commendedLeader);
    m_level = config.GetNumber("player_level", m_level);
    m_xp = config.GetNumber("player_cur_xp", m_xp);

    m_country = config.GetString("country", m_country);
    m_currency = config.GetNumber("currency", m_currency);

    // fatal error string, shown by the client in a popup instead of ClientWelcome
    m_error = config.GetString("error", m_error);

    if (!m_hasPrime)
    {
        m_competitiveRank = RankNone;
        m_competitiveWins = 0;
        m_wingmanRank = RankNone;
        m_wingmanWins = 0;
        m_dangerZoneRank = DangerZoneRankNone;
        m_dangerZoneWins = 0;
        m_level = 0;
        m_xp = 0;
    }
}

GCConfig::GCConfig()
{
    KeyValue config{ "config" };
    if (!config.ParseFromFile(ConfigFilePath))
        return;
    Parse(config);
}

void GCConfig::ReloadFromFile()
{
    KeyValue config{ "config" };
    if (!config.ParseFromFile(ConfigFilePath))
        return;
    Parse(config);
}

// Helper: find `"key"` in a line, then the next quoted string after it,
// and replace its contents in-place. Preserves indentation, tabs, trailing
// comments, etc.
static bool ReplaceQuotedValueInLine(std::string &line, std::string_view key, std::string_view newValue)
{
    const std::string needle = "\"" + std::string(key) + "\"";

    size_t keyPos = line.find(needle);
    if (keyPos == std::string::npos)
        return false;

    size_t valStart = line.find('"', keyPos + needle.size());
    if (valStart == std::string::npos)
        return false;

    size_t valEnd = line.find('"', valStart + 1);
    if (valEnd == std::string::npos)
        return false;

    line.replace(valStart + 1, valEnd - valStart - 1, newValue);
    return true;
}

void GCConfig::Save() const
{
    // Load the file as plain text so we keep comments, tabs, blank lines,
    // and key ordering intact. We only rewrite two values in-place.
    std::string data = LoadFile(ConfigFilePath);

    bool foundLevel = false;
    bool foundXp = false;

    std::string out;
    out.reserve(data.size() + 64);

    size_t pos = 0;
    while (pos < data.size())
    {
        size_t eol = data.find('\n', pos);
        bool hasNewline = (eol != std::string::npos);
        if (!hasNewline)
            eol = data.size();

        std::string line = data.substr(pos, eol - pos);

        if (!foundLevel && ReplaceQuotedValueInLine(line, "player_level", std::to_string(m_level)))
        {
            foundLevel = true;
        }
        else if (!foundXp && ReplaceQuotedValueInLine(line, "player_cur_xp", std::to_string(m_xp)))
        {
            foundXp = true;
        }

        out += line;
        if (hasNewline)
            out += '\n';

        pos = eol + (hasNewline ? 1 : 0);
    }

    // If either key was missing from the file, insert it just before the
    // last closing brace so it still lives inside the "config" block.
    if (!foundLevel || !foundXp)
    {
        std::string insert;
        if (!foundLevel)
            insert += "\t\"player_level\"\t\t\"" + std::to_string(m_level) + "\"\n";
        if (!foundXp)
            insert += "\t\"player_cur_xp\"\t\t\"" + std::to_string(m_xp) + "\"\n";

        size_t closePos = out.rfind('}');
        if (closePos != std::string::npos)
            out.insert(closePos, insert);
        else
            out += insert; // no closing brace? just append
    }

    FILE *f = fopen(ConfigFilePath, "wb");
    if (!f)
    {
        Platform::Print("GCConfig::Save: failed to open %s for writing\n", ConfigFilePath);
        return;
    }

    fwrite(out.data(), 1, out.size(), f);
    fclose(f);

    Platform::Print("config.txt updated: player_level=%d player_cur_xp=%d\n", m_level, m_xp);
}

float GCConfig::GetRarityWeight(uint32_t rarity) const
{
    for (const RarityWeight &weight : m_rarityWeights)
    {
        if (weight.rarity == rarity)
        {
            return weight.weight;
        }
    }

    return 0;
}

GCConfig &GetConfig()
{
    static GCConfig config;
    return config;
}
