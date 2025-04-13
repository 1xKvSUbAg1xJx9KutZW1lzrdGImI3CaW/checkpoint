#include "Db.hpp"

#include "../GlobalState.hpp"
#include "../debug/log.hpp"
#include "../config/Config.hpp"

#include <filesystem>
#include <string_view>
#include <algorithm>
#include <fstream>

constexpr const char*    DB_FILE                    = "data.db";
constexpr const uint64_t DB_TIME_BEFORE_CLEANUP_MS  = 1000 * 60 * 10; // 10 mins
constexpr const uint64_t DB_TOKEN_LIFE_LENGTH_S     = 60 * 60;        // 1hr
constexpr const uint64_t DB_CHALLENGE_LIFE_LENGTH_S = 60 * 10;        // 10 mins
constexpr const uint64_t DB_SCHEMA_VERSION          = 2;

//
static std::string readFileAsText(const std::string& path) {
    std::ifstream ifs(path);
    auto          res = std::string((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
    if (res.back() == '\n')
        res.pop_back();
    return res;
}

static std::string dbDir() {
    static const std::string dir = std::filesystem::canonical(g_pGlobalState->cwd + "/" + g_pConfig->m_config.data_dir).string();
    return dir;
}

static std::string dbPath() {
    static const std::string path = dbDir() + "/" + DB_FILE;
    return path;
}

static bool isHashValid(const std::string_view sv) {
    return std::all_of(sv.begin(), sv.end(), [](const char& c) { return (c >= 'a' && c <= 'f') || std::isdigit(c); });
}

CDatabase::CDatabase() {
    if (!std::filesystem::exists(dbDir())) {
        Debug::log(LOG, "Data dir doesn't exist, creating.");
        std::filesystem::create_directory(dbDir());
    }

    if (std::filesystem::exists(dbPath())) {
        if (std::filesystem::exists(dbDir() + "/schema")) {
            int schema = std::stoi(readFileAsText(dbDir() + "/schema"));
            if (schema == DB_SCHEMA_VERSION) {
                if (sqlite3_open(dbPath().c_str(), &m_db) != SQLITE_OK)
                    throw std::runtime_error("failed to open sqlite3 db");

                cleanupDb();
                return;
            } else
                Debug::log(LOG, "Database outdated, recreating db");
        } else
            Debug::log(LOG, "Database schema not present, recreating db");
    } else
        Debug::log(LOG, "Database not present, creating one");

    std::filesystem::remove(dbPath());

    if (sqlite3_open(dbPath().c_str(), &m_db) != SQLITE_OK)
        throw std::runtime_error("failed to open sqlite3 db");

    std::ofstream of(dbDir() + "/schema", std::ios::trunc);
    of << DB_SCHEMA_VERSION;
    of.close();

    // create db layout
    char*       errmsg = nullptr;

    const char* CHALLENGE_TABLE = R"#(
CREATE TABLE challenges (
	nonce TEXT NOT NULL,
	fingerprint TEXT NOT NULL,
	difficulty INTEGER NOT NULL,
	epoch INTEGER NOT NULL,
	CONSTRAINT PK PRIMARY KEY (nonce)
);)#";

    sqlite3_exec(m_db, CHALLENGE_TABLE, [](void* data, int len, char** a, char** b) -> int { return 0; }, nullptr, &errmsg);

    const char* TOKENS_TABLE = R"#(
CREATE TABLE tokens (
	token TEXT NOT NULL,
	fingerprint TEXT NOT NULL,
	epoch INTEGER NOT NULL,
	CONSTRAINT PK PRIMARY KEY (token)
);)#";

    sqlite3_exec(m_db, TOKENS_TABLE, [](void* data, int len, char** a, char** b) -> int { return 0; }, nullptr, &errmsg);
}

CDatabase::~CDatabase() {
    if (m_db)
        sqlite3_close(m_db);
}

void CDatabase::addChallenge(const SDatabaseChallengeEntry& entry) {
    if (!isHashValid(entry.nonce))
        return;

    if (!isHashValid(entry.fingerprint))
        return;

    const std::string CMD = fmt::format(R"#(
INSERT INTO challenges VALUES (
"{}", "{}", {}, {}
);)#",
                                        entry.nonce, entry.fingerprint, entry.difficulty, entry.epoch);

    char*             errmsg = nullptr;
    sqlite3_exec(m_db, CMD.c_str(), nullptr, nullptr, &errmsg);

    if (errmsg)
        Debug::log(ERR, "sqlite3 error: tried to persist:\n{}\nGot: {}", CMD, errmsg);
}

std::optional<SDatabaseChallengeEntry> CDatabase::getChallenge(const std::string& nonce) {
    if (!isHashValid(nonce))
        return std::nullopt;

    const std::string       CMD = fmt::format(R"#(
SELECT * FROM challenges WHERE nonce = "{}";
)#",
                                              nonce);

    char*                   errmsg = nullptr;
    CDatabase::SQueryResult result;

    sqlite3_exec(
        m_db, CMD.c_str(),
        [](void* result, int len, char** a, char** b) -> int {
            auto res = reinterpret_cast<CDatabase::SQueryResult*>(result);

            for (size_t i = 0; i < len; ++i) {
                res->result.push_back(a[i]);
                res->result2.push_back(b[i]);
            }

            return 0;
        },
        &result, &errmsg);

    if (errmsg || result.result.size() < 4)
        return std::nullopt;

    return SDatabaseChallengeEntry{.nonce = nonce, .difficulty = std::stoi(result.result.at(2)), .epoch = std::stoull(result.result.at(3)), .fingerprint = result.result.at(1)};
}

void CDatabase::dropChallenge(const std::string& nonce) {
    if (!isHashValid(nonce))
        return;

    const std::string CMD = fmt::format(R"#(
DELETE FROM tokens WHERE token = "{}"
)#",
                                        nonce);

    char*             errmsg = nullptr;
    sqlite3_exec(m_db, CMD.c_str(), nullptr, nullptr, &errmsg);

    if (errmsg)
        Debug::log(ERR, "sqlite3 error: tried to persist:\n{}\nGot: {}", CMD, errmsg);
}

void CDatabase::addToken(const SDatabaseTokenEntry& entry) {
    if (!isHashValid(entry.token))
        return;

    if (!isHashValid(entry.fingerprint))
        return;

    const std::string CMD = fmt::format(R"#(
INSERT INTO tokens VALUES (
"{}", "{}", {}
);)#",
                                        entry.token, entry.fingerprint, entry.epoch);

    char*             errmsg = nullptr;
    sqlite3_exec(m_db, CMD.c_str(), nullptr, nullptr, &errmsg);

    if (errmsg)
        Debug::log(ERR, "sqlite3 error: tried to persist:\n{}\nGot: {}", CMD, errmsg);
}

void CDatabase::dropToken(const std::string& token) {
    if (!isHashValid(token))
        return;

    const std::string CMD = fmt::format(R"#(
DELETE FROM tokens WHERE token = "{}"
)#",
                                        token);

    char*             errmsg = nullptr;
    sqlite3_exec(m_db, CMD.c_str(), nullptr, nullptr, &errmsg);

    if (errmsg)
        Debug::log(ERR, "sqlite3 error: tried to persist:\n{}\nGot: {}", CMD, errmsg);
}

std::optional<SDatabaseTokenEntry> CDatabase::getToken(const std::string& token) {
    if (!isHashValid(token))
        return std::nullopt;

    if (shouldCleanupDb())
        cleanupDb();

    const std::string       CMD = fmt::format(R"#(
SELECT * FROM tokens WHERE token = "{}";
)#",
                                              token);

    char*                   errmsg = nullptr;
    CDatabase::SQueryResult result;

    sqlite3_exec(
        m_db, CMD.c_str(),
        [](void* result, int len, char** a, char** b) -> int {
            auto res = reinterpret_cast<CDatabase::SQueryResult*>(result);

            for (size_t i = 0; i < len; ++i) {
                res->result.push_back(a[i]);
                res->result2.push_back(b[i]);
            }

            return 0;
        },
        &result, &errmsg);

    if (errmsg || result.result.size() < 3)
        return std::nullopt;

    return SDatabaseTokenEntry{.token = token, .epoch = std::stoull(result.result.at(2)), .fingerprint = result.result.at(1)};
}

bool CDatabase::shouldCleanupDb() {
    const auto TIME = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto LAST = std::chrono::duration_cast<std::chrono::milliseconds>(m_lastDbCleanup.time_since_epoch()).count();

    if (TIME - LAST > DB_TIME_BEFORE_CLEANUP_MS)
        return true;

    return false;
}

void CDatabase::cleanupDb() {
    m_lastDbCleanup = std::chrono::steady_clock::now();

    const auto  TIME = std::chrono::milliseconds(std::time(nullptr)).count();

    std::string CMD = fmt::format(R"#(
DELETE FROM tokens WHERE epoch < {};
)#",
                                  TIME - DB_TOKEN_LIFE_LENGTH_S);

    char*       errmsg = nullptr;
    sqlite3_exec(m_db, CMD.c_str(), nullptr, nullptr, &errmsg);

    if (errmsg)
        Debug::log(ERR, "sqlite3 error: tried to persist:\n{}\nGot: {}", CMD, errmsg);

    CMD = fmt::format(R"#(
DELETE FROM challenges WHERE epoch < {};
)#",
                      TIME - DB_CHALLENGE_LIFE_LENGTH_S);

    sqlite3_exec(m_db, CMD.c_str(), nullptr, nullptr, &errmsg);

    if (errmsg)
        Debug::log(ERR, "sqlite3 error: tried to persist:\n{}\nGot: {}", CMD, errmsg);
}
