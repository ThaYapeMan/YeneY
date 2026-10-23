// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_lms_cli.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
namespace bridge {
// JSON validation and semantic recognition of the one-property icon objects
// YeneY writes. Other valid JSON values are owner selections, never ours.
class IconJson {
    const std::string &s;
    size_t at = 0;
    void space() {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n'))
            ++at;
    }
    bool eat(char c) {
        space();
        if (at < s.size() && s[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    [[noreturn]] void bad() { throw std::runtime_error("malformed icon JSON"); }
    unsigned hex4() {
        unsigned n = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (at == s.size())
                bad();
            const char c = s[at++];
            int v = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
            if (v < 0)
                bad();
            n = n * 16 + v;
        }
        return n;
    }
    static void utf8(std::string &out, unsigned n) {
        if (n < 128)
            out += char(n);
        else if (n < 2048) {
            out += char(192 | (n >> 6));
            out += char(128 | (n & 63));
        } else if (n < 65536) {
            out += char(224 | (n >> 12));
            out += char(128 | ((n >> 6) & 63));
            out += char(128 | (n & 63));
        } else {
            out += char(240 | (n >> 18));
            out += char(128 | ((n >> 12) & 63));
            out += char(128 | ((n >> 6) & 63));
            out += char(128 | (n & 63));
        }
    }
    std::string string() {
        if (!eat('"'))
            bad();
        std::string out;
        while (at < s.size()) {
            unsigned char c = s[at++];
            if (c == '"')
                return out;
            if (c < 32)
                bad();
            if (c == '\\') {
                if (at == s.size())
                    bad();
                c = s[at++];
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    out += char(c);
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned n = hex4();
                    if (n >= 0xd800 && n <= 0xdbff) {
                        if (at + 2 > s.size() || s.substr(at, 2) != "\\u")
                            bad();
                        at += 2;
                        unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff)
                            bad();
                        n = 0x10000 + (n - 0xd800) * 1024 + low - 0xdc00;
                    } else if (n >= 0xdc00 && n <= 0xdfff)
                        bad();
                    utf8(out, n);
                    break;
                }
                default:
                    bad();
                }
            } else if (c < 128)
                out += char(c);
            else {
                unsigned n = 0, count = 0, min = 0;
                if (c >= 194 && c <= 223) {
                    n = c & 31;
                    count = 1;
                    min = 128;
                } else if (c >= 224 && c <= 239) {
                    n = c & 15;
                    count = 2;
                    min = 2048;
                } else if (c >= 240 && c <= 244) {
                    n = c & 7;
                    count = 3;
                    min = 65536;
                } else
                    bad();
                for (unsigned i = 0; i < count; ++i) {
                    if (at == s.size())
                        bad();
                    unsigned b = static_cast<unsigned char>(s[at++]);
                    if ((b & 192) != 128)
                        bad();
                    n = (n << 6) | (b & 63);
                }
                if (n < min || n > 0x10ffff || (n >= 0xd800 && n <= 0xdfff))
                    bad();
                utf8(out, n);
            }
        }
        bad();
    }
    std::optional<std::string> value(unsigned depth) {
        if (depth > 32)
            bad();
        space();
        if (at == s.size())
            bad();
        if (s[at] == '{') {
            ++at;
            std::set<std::string> keys;
            std::optional<std::string> icon;
            if (!eat('}')) {
                do {
                    auto key = string();
                    if (!keys.insert(key).second || !eat(':'))
                        bad();
                    space();
                    bool text = at < s.size() && s[at] == '"';
                    if (text) {
                        auto v = string();
                        if (key == "svg")
                            icon = v;
                    } else
                        value(depth + 1);
                } while (eat(','));
                if (!eat('}'))
                    bad();
            }
            return keys.size() == 1 && keys.count("svg") ? icon : std::nullopt;
        }
        if (s[at] == '[') {
            ++at;
            if (!eat(']')) {
                do {
                    value(depth + 1);
                } while (eat(','));
                if (!eat(']'))
                    bad();
            }
            return {};
        }
        if (s[at] == '"') {
            string();
            return {};
        }
        for (auto word : {"null", "true", "false"}) {
            size_t n = std::char_traits<char>::length(word);
            if (s.compare(at, n, word) == 0) {
                at += n;
                return {};
            }
        }
        if (s[at] == '-')
            ++at;
        if (at == s.size())
            bad();
        if (s[at] == '0')
            ++at;
        else {
            if (s[at] < '1' || s[at] > '9')
                bad();
            while (at < s.size() && s[at] >= '0' && s[at] <= '9')
                ++at;
        }
        if (at < s.size() && s[at] == '.') {
            ++at;
            auto start = at;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9')
                ++at;
            if (at == start)
                bad();
        }
        if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
            ++at;
            if (at < s.size() && (s[at] == '+' || s[at] == '-'))
                ++at;
            auto start = at;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9')
                ++at;
            if (at == start)
                bad();
        }
        return {};
    }

public:
    explicit IconJson(const std::string &text) : s(text) {}
    std::optional<std::string> parse() {
        auto v = value(0);
        space();
        if (at != s.size())
            bad();
        return v;
    }
};
inline std::string iconEncode(const std::string &s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline std::vector<std::string> iconTokens(const std::string &raw) {
    std::istringstream input(raw);
    std::vector<std::string> values;
    std::string token;
    while (input >> token)
        values.push_back(queue_audio::decodeLms(token));
    return values;
}
class LmsIcon {
public:
    using Log = std::function<void(const std::string &)>;

private:
    std::string mac, name, directory;
    Log log;
    unsigned port;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> jobs;
    std::atomic_bool cancel{false};
    bool stopping = false, enabled = true;
    std::thread worker;
    static constexpr const char *key = "plugin.material-skin:icon";
    void save(const std::filesystem::path &path, const std::string &json) {
        auto temporary = path.string() + ".XXXXXX";
        std::vector<char> bytes(temporary.begin(), temporary.end());
        bytes.push_back(0);
        int fd = mkstemp(bytes.data());
        if (fd < 0)
            throw std::runtime_error("state unavailable");
        bool ok = ::write(fd, json.data(), json.size()) == ssize_t(json.size()) && fsync(fd) == 0;
        if (close(fd) != 0)
            ok = false;
        if (ok)
            ok = ::rename(bytes.data(), path.c_str()) == 0;
        if (!ok) {
            unlink(bytes.data());
            throw std::runtime_error("state write failed");
        }
    }
    std::string apply(const std::string &host) {
        if (directory.empty() || !std::filesystem::is_directory(directory))
            return "state directory unavailable";
        std::string reply;
        auto call = [&](const std::string &cmd, bool global = false, bool empty = false,
                        unsigned timeout = 3000) {
            return queue_audio::controlLms(host, mac, cmd, port, &reply, timeout, &cancel, global, empty);
        };
        if (!call("players 0 1000", true))
            return "players query failed";
        auto players = iconTokens(reply);
        bool visible = false;
        for (auto token : players) {
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (token == "playerid:" + mac)
                visible = true;
        }
        if (!visible)
            return "player not visible";
        const auto path = std::filesystem::path(directory) / ("material-icon-" + mac + ".json");
        std::optional<std::string> previous;
        if (std::filesystem::exists(path)) {
            if (std::filesystem::is_symlink(path))
                return "state symlink refused";
            std::ifstream file(path);
            std::string text((std::istreambuf_iterator<char>(file)), {});
            previous = IconJson(text).parse();
            if (!previous)
                return "invalid state";
        }
        if (!call(std::string("playerpref ") + key + " ?", false, true))
            return "preference query failed";
        auto tokens = iconTokens(reply);
        if (tokens.size() < 3 || tokens.size() > 4)
            return "malformed preference reply";
        auto current = tokens.size() == 4 ? tokens.back() : std::string{};
        std::optional<std::string> selected;
        if (!current.empty())
            selected = IconJson(current).parse();
        if (selected && *selected == name)
            return "already configured";
        if (!current.empty() && (!selected || !previous || *selected != *previous))
            return "owner selection preserved";
        auto json = "{\"svg\":\"" + name + "\"}";
        if (!call(std::string("playerpref ") + key + " " + iconEncode(json), false, false, 15000))
            return "write unconfirmed";
        save(path, json);
        return "set svg=" + name;
    }
    void run() {
        while (true) {
            std::string host;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping)
                    return;
                host = std::move(jobs.front());
                jobs.pop_front();
                cancel = false;
            }
            std::string outcome;
            try {
                outcome = apply(host);
            } catch (const std::exception &) {
                outcome = "invalid response or state";
            }
            log("LMS icon: " + outcome);
        }
    }

public:
    LmsIcon(const uint8_t *address, Log logger, unsigned cliPort = 9090)
        : log(std::move(logger)), port(cliPort) {
        char text[18];
        std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1], address[2],
                      address[3], address[4], address[5]);
        mac = text;
        const char *setting = std::getenv("YENEY_LMS_ICON");
        name = setting ? setting : "sonos";
        enabled = !name.empty();
        if (name.size() > 64 ||
            name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos) {
            enabled = false;
            log("LMS icon: warning invalid YENEY_LMS_ICON; automatic handling disabled");
        }
        if (const char *state = std::getenv("STATE_DIRECTORY"))
            directory = state;
        if (!directory.empty() &&
            (!std::filesystem::path(directory).is_absolute() || directory.find(':') != std::string::npos))
            directory.clear();
        if (enabled)
            worker = std::thread([this] { run(); });
    }
    ~LmsIcon() { shutdown(); }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            cancel = true;
        }
        changed.notify_all();
        if (worker.joinable())
            worker.join();
    }
    void connected(const std::string &host) {
        if (!enabled) {
            log("LMS icon: automatic handling disabled");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        cancel = true;
        while (!jobs.empty()) {
            jobs.pop_front();
            log("LMS icon: connection superseded");
        }
        jobs.push_back(host);
        changed.notify_all();
    }
};
} // namespace bridge
