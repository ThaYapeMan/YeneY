// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_position.h"
#include <cassert>
#include <fstream>
#include <iostream>

namespace {
struct Answer {
    double sent;
    double received;
    double position;
    std::string item;
    std::string state;
    double reading;
    double lmsPosition;

    double midpoint() const { return (sent + received) / 2; }
    double rtt() const { return (received - sent) * 1000; }
    double project(double at) const {
        return position + (bridge::speakerPlaying(state) ? (at - midpoint()) * 1000 : 0);
    }
    bridge::SpeakerPositionSample sample() const {
        return {item, state, position, midpoint(), rtt()};
    }
};

std::vector<Answer> study() {
    std::ifstream fixture("tests/fixtures/timing-study-replay.tsv");
    std::string columns;
    std::getline(fixture, columns);
    assert(columns == "sent_us\treceived_us\tposition_ms\titem\tstate\tlms_midpoint_us\tlms_position_ms");
    std::vector<Answer> answers;
    Answer row;
    while (fixture >> row.sent >> row.received >> row.position >> row.item >> row.state >> row.reading >> row.lmsPosition) {
        row.sent /= 1000000;
        row.received /= 1000000;
        row.reading /= 1000000;
        if (row.state == "BUFFERING") row.state = "TRANSITIONING";
        answers.push_back(row);
    }
    assert(fixture.eof() && answers.size() == 480);
    return answers;
}

double median(std::vector<double> values) {
    assert(!values.empty());
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

void ownerPairing() {
    std::vector<double> errors;
    size_t within = 0;
    for (const auto &answer : study()) {
        if (answer.rtt() > 200) continue;
        const double error = answer.project(answer.reading) - answer.lmsPosition;
        within += std::abs(error) <= 30;
        errors.push_back(error);
    }
    const double middle = median(errors);
    std::cout << "Study paired baseline samples=" << errors.size() << " within_30_pct="
              << 100.0 * within / errors.size() << " median_ms=" << middle << '\n';
    assert(errors.size() == 465 && within == 430);
    assert(std::abs(middle - 2.428) < .003);
}

void recordedTransitions() {
    const auto answers = study();
    const auto skip = std::find_if(answers.begin(), answers.end(), [](const auto &answer) {
        return answer.item == "3";
    });
    assert(skip != answers.end() && skip->state == "TRANSITIONING" && skip->position == 0);
    assert(skip[1].lmsPosition == 0 && skip[2].lmsPosition == 0 && skip[3].lmsPosition == 0);
    assert(skip[3].position == 767 && std::abs(skip[4].project(skip[4].reading) - skip[4].lmsPosition) <= 30);
    const auto scrub = std::find_if(skip + 1, answers.end(), [](const auto &answer) {
        return answer.state == "TRANSITIONING";
    });
    assert(scrub != answers.end() && scrub->position == 83375 && scrub[1].position == scrub->position);
    for (int i = 2; i <= 4; ++i) {
        const double ahead = scrub[i].lmsPosition - scrub[i].project(scrub[i].reading);
        assert(ahead >= 160 && ahead <= 190);
    }
    assert(std::abs(scrub[5].project(scrub[5].reading) - scrub[5].lmsPosition) <= 30);
    std::cout << "Study paired baseline first inside 30 ms skip_ms="
              << (skip[4].reading - skip->midpoint()) * 1000
              << " scrub_ms=" << (scrub[5].reading - scrub->midpoint()) * 1000 << '\n';
}

void studyFit() {
    bridge::PositionFit fit;
    std::vector<double> errors;
    size_t within = 0;
    for (const auto &answer : study()) {
        fit.add(answer.sample());
        // Contract replay has position answers only. LMS was driven by a
        // separate, faster connection, whose event and answer trace is absent.
        if (answer.received < 46 || answer.received > 116) continue;
        const double truth = answer.received * 1000 + 64033.27;
        const double error = fit.position(answer.received)->positionMs - truth;
        errors.push_back(error);
        within += std::abs(error) <= 30;
    }
    assert(errors.size() >= 279);
    std::cout << "Study fit replay samples=" << errors.size() << " within_30_pct="
              << 100.0 * within / errors.size() << " median_ms=" << median(errors) << '\n';
}

void portFit() {
    bridge::PositionFit fit;
    const double origin = 10000;
    double sent = 0;
    for (int i = 0; i < 1534; ++i) {
        const double rtt = i == 767 ? .307748 : i % 20 ? .0105944 : .0248434;
        const double received = sent + rtt;
        const double truth = origin + received * 1000;
        const Answer answer{sent, received, origin + (sent + rtt / 2) * 1000 + (i == 767 ? 110.403 : 1.169),
                            "1", "PLAYING", received, truth};
        assert(fit.add(answer.sample()) == (i != 767));
        assert(std::abs(fit.position(received)->positionMs - truth) <= 1.171);
        sent = std::max(sent + .05, received);
    }
    std::cout << "Port controlled fit rejects 307.748 ms answer and retains the line\n";
}
} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "paired") ownerPairing();
    else if (mode == "changes") recordedTransitions();
    else if (mode == "study") studyFit();
    else if (mode == "port") portFit();
    else return 2;
}
