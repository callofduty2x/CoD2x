#ifndef MATCH_H
#define MATCH_H

#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "cod2_server.h"
#include "http_client.h"
#include "server.h"

#define MAX_TEAM_PLAYERS (MAX_CLIENTS / 2)
#define MAX_ID_LENGTH 64
#define MAX_NAME_LENGTH 32
#define MAX_MAP_NAME_LENGTH 32
#define MAX_MAPS 5


template <typename K, typename V>
class ordered_map {
    std::unordered_map<K, V> m_map;
    std::vector<K> m_order;

public:
    // Insert or update
    V& operator[](const K& key) {
        auto [it, inserted] = m_map.emplace(key, V{});
        if (inserted) {
            m_order.push_back(key); // remember insertion order
        }
        return it->second;
    }

    bool contains(const K& key) const {
        return m_map.find(key) != m_map.end();
    }

    bool erase(const K& key) {
        auto it = m_map.find(key);
        if (it == m_map.end()) return false;
        m_map.erase(it);
        m_order.erase(std::remove(m_order.begin(), m_order.end(), key), m_order.end());
        return true;
    }

    V& at(const K& key) { return m_map.at(key); }
    const V& at(const K& key) const { return m_map.at(key); }
    const std::vector<K>& keys() const { return m_order; }

    void clear() {
        m_map.clear();
        m_order.clear();
    }
};


typedef struct {
    // Key - value of global data
    // It will contain information like "map", "team1_score", etc.
    ordered_map<std::string, std::string> globalData;

    // Key - value of players where key is player's UUID, but might be empty
    // It will contain information like "kills", "deaths", etc.
    ordered_map<std::string, ordered_map<std::string, std::string>> playerData;
} MatchProgressData;


typedef struct {
    char id[MAX_ID_LENGTH];
    char name[MAX_NAME_LENGTH];
    ordered_map<std::string, std::string> otherData;

    // Local data
    int teamNumber;                 // filled automatically
    char teamName[MAX_NAME_LENGTH]; // filled automatically
} MatchPlayer;

typedef struct {
    char id[MAX_ID_LENGTH];
    char name[MAX_NAME_LENGTH];
    MatchPlayer players[MAX_TEAM_PLAYERS];
    int num_players;
    ordered_map<std::string, std::string> otherData;
} MatchTeam;

typedef struct {
    // Data from server
    char match_id[MAX_ID_LENGTH];
    MatchTeam team1;
    MatchTeam team2;
    char maps[MAX_MAPS][MAX_MAP_NAME_LENGTH]; // Up to 5 maps
    int maps_count;
    ordered_map<std::string, std::string> otherData;

    // Local data
    std::string json;
    std::string error;
} MatchData;

typedef struct {
    bool downloading;
    bool loading;
    bool activated;
    bool uploading;
    bool uploadingError;
    bool canceling;
    char cancelReason[256];
    char url[256]; // URL to the match server
    HttpClient* httpClient;
    uint64_t start_time; // Time when the match data download started
    uint64_t start_tick; // Tick when the match data download started

    // Data from server
    MatchData data;

    // Match progress data
    MatchProgressData progressData;

} Match;

extern Match match;

bool match_upload_match_data(std::function<void()> onDone = nullptr, std::function<void(const std::string&)> onError = nullptr);
MatchPlayer* match_find_player_by_uuid(const char* uuid);
bool match_redownload();
void match_cancel(const char* reason);
void match_finish();
bool match_beforeMapChangeOrRestart(bool fromScript, bool bComplete, bool shutdown, sv_map_change_source_e source);
void match_onStartGameType();
void match_frame();
void match_init_client();
void match_init();
void match_patch();

#endif 