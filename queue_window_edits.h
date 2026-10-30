// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "queue_window.h"
#include <functional>
#include <set>

namespace queue_audio {
// Execute parent-window edits through the existing queue SOAP boundary. A
// parent may own multiple continuation URIs; all its segments move together.
// No retry after an uncertain reply: the room must resolve/release ownership.
class WindowEdits {
public:
    struct Parent { QueueWindow::Token token; std::vector<std::string> uris; };
    struct Snapshot { uint32_t update; std::vector<std::string> uris; };
    struct IO {
        std::function<Snapshot()> browse;
        std::function<void(uint32_t, unsigned, unsigned)> remove;
        // before is a one-based insertion ordinal, including an append at N+1.
        std::function<void(QueueWindow::Token, unsigned, unsigned)> insert;
        std::function<void(const std::string &)> log;
    };
private:
    IO io;
    static std::vector<std::string> flatten(const std::vector<Parent> &parents) {
        std::vector<std::string> uris;
        std::set<std::string> seen;
        for (const auto &parent : parents) {
            if (parent.uris.empty()) throw std::invalid_argument("window parent has no URI");
            for (const auto &uri : parent.uris) {
                if (uri.empty() || !seen.insert(uri).second) throw std::invalid_argument("window URI reused");
                uris.push_back(uri);
            }
        }
        return uris;
    }
public:
    explicit WindowEdits(IO callbacks) : io(std::move(callbacks)) {
        if (!io.browse || !io.remove || !io.insert) throw std::invalid_argument("window edit IO incomplete");
    }
    void apply(std::vector<Parent> current, const std::vector<Parent> &wanted,
               std::optional<QueueWindow::Token> anchor = {}) {
        auto expected = flatten(current);
        (void)flatten(wanted);
        std::vector<QueueWindow::Token> from, to;
        for (const auto &parent : current) from.push_back(parent.token);
        for (const auto &parent : wanted) {
            to.push_back(parent.token);
            const auto old = std::find_if(current.begin(), current.end(),
                [&](const auto &p) { return p.token == parent.token; });
            if (old != current.end() && old->uris != parent.uris)
                throw std::invalid_argument("continuation change needs explicit segment insertion");
        }
        const auto plan = QueueWindow::edits(from, to, anchor);
        const auto verify = [&](const Snapshot &snapshot) {
            if (snapshot.uris != expected) throw std::runtime_error("queue window ownership changed");
        };
        // Even a no-op plan must confirm ownership of the whole URI vector.
        verify(io.browse());
        for (const auto &edit : plan) {
            unsigned ordinal = 1;
            for (unsigned n = 0; n + 1 < edit.ordinal; ++n) ordinal += current.at(n).uris.size();
            const auto before = io.browse();
            verify(before);
            if (edit.kind == QueueWindow::Edit::Remove) {
                const auto &parent = current.at(edit.ordinal - 1);
                if (parent.token != edit.token) throw std::logic_error("window plan drift");
                const auto count = unsigned(parent.uris.size());
                io.remove(before.update, ordinal, count);
                expected.erase(expected.begin() + ordinal - 1, expected.begin() + ordinal - 1 + count);
                const auto after = io.browse();
                verify(after);
                if (after.update == before.update) throw std::runtime_error("queue window removal unconfirmed");
                current.erase(current.begin() + edit.ordinal - 1);
                if (io.log) io.log("queue window remove ordinal=" + std::to_string(ordinal) + " count=" + std::to_string(count));
            } else {
                const auto found = std::find_if(wanted.begin(), wanted.end(),
                    [&](const auto &parent) { return parent.token == edit.token; });
                if (found == wanted.end()) throw std::logic_error("window insertion missing");
                auto update = before.update;
                for (unsigned segment = 0; segment < found->uris.size(); ++segment) {
                    // AddURIToQueue has no UpdateID input. A fresh Browse and
                    // exact vector verification surround each single enqueue.
                    const auto latest = io.browse();
                    verify(latest);
                    if (latest.update != update) throw std::runtime_error("queue window revision changed");
                    io.insert(edit.token, segment, ordinal + segment);
                    expected.insert(expected.begin() + ordinal - 1 + segment, found->uris[segment]);
                    const auto after = io.browse();
                    verify(after);
                    if (after.update == update) throw std::runtime_error("queue window insertion unconfirmed");
                    update = after.update;
                }
                current.insert(current.begin() + edit.ordinal - 1, *found);
                if (io.log) io.log("queue window insert ordinal=" + std::to_string(ordinal) + " count=" + std::to_string(found->uris.size()));
            }
        }
    }
};
} // namespace queue_audio
