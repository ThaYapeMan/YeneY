// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
namespace timing_probe {
// One file per UDN; hexadecimal encoding avoids path traversal and collisions.
class DriftState {
    std::string path;
    bool warned = false;
    void warning(const char *reason) {
        if (!warned) fprintf(stderr, "yeney: drift-state path=%s unavailable=%s; continuing without persistence\n", path.c_str(), reason);
        warned = true;
    }
public:
    struct Value { double ppm = 0, sigma = 0; long long timestamp = 0; };
    static bool valid(Value v, long long now) {
        return std::isfinite(v.ppm) && std::abs(v.ppm) <= 500 &&
               std::isfinite(v.sigma) && v.sigma > 0 && v.sigma <= 15 &&
               v.timestamp <= now && now - v.timestamp <= 7 * 86400;
    }
    void speaker(const std::string &udn, const std::string &directory = "/var/lib/yeney") {
        path = directory + "/drift-";
        for (unsigned char c : udn) { char hex[3]; snprintf(hex, sizeof hex, "%02x", c); path += hex; }
        path += ".state";
    }
    bool load(Value &v, long long now = time(nullptr)) {
        FILE *f = fopen(path.c_str(), "r");
        if (!f) {
            struct stat st{};
            const auto directory = path.substr(0, path.rfind('/'));
            if (stat(directory.c_str(), &st) || !(st.st_mode & 0222)) warning("missing-or-read-only-directory");
            return false;
        }
        int version = 0;
        const bool parsed = fscanf(f, "%d %lf %lf %lld", &version, &v.ppm, &v.sigma, &v.timestamp) == 4;
        fclose(f);
        return parsed && version == 1 && valid(v, now);
    }
    bool save(Value v) {
        if (path.empty() || warned) return false;
        struct stat st{};
        const auto directory = path.substr(0, path.rfind('/'));
        if (stat(directory.c_str(), &st) || !(st.st_mode & 0222)) {
            warning("missing-or-read-only-directory"); return false;
        }
        std::string temporary = path + ".XXXXXX";
        int fd = mkstemp(&temporary[0]);
        if (fd < 0) { warning("create"); return false; }
        char data[160];
        int length = snprintf(data, sizeof data, "1 %.9f %.9f %lld\n", v.ppm, v.sigma, v.timestamp);
        bool ok = write(fd, data, length) == length && fsync(fd) == 0;
        if (close(fd)) ok = false;
        if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0;
        if (!ok) { unlink(temporary.c_str()); warning("atomic-write"); }
        return ok;
    }
};
}
