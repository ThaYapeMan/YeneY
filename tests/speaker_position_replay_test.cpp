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
    bridge::PositionLink link;
    const double origin = 10000;
    double sent = 0;
    for (int i = 0; i < 1534; ++i) {
        const double rtt = i == 767 ? .307748 : i % 20 ? .0105944 : .0248434;
        const double received = sent + rtt;
        const double truth = origin + received * 1000;
        const Answer answer{sent, received, origin + (sent + rtt / 2) * 1000 + (i == 767 ? 110.403 : 1.169),
                            "1", "PLAYING", received, truth};
        assert(!link.observe(received, answer.rtt()) && !link.isUneven());
        assert(fit.add(answer.sample()) == (i != 767));
        assert(std::abs(fit.position(received)->positionMs - truth) <= 1.171);
        sent = std::max(sent + .05, received);
    }
    std::cout << "Port controlled fit rejects 307.748 ms answer and retains the line\n";
}

void linkClassification() {
    bridge::PositionLink link;
    double now = 0;
    for (int i = 0; i < 12; ++i, now += .05) assert(!link.observe(now, 10));
    assert(!link.observe(now += .15, 140));
    assert(!link.observe(now += .15, 140));
    assert(link.observe(now += .15, 140) && link.isUneven() && link.trustedLimit() == 22);
    for (int i = 0; i < 200; ++i) assert(!link.observe(now += .05, 10));
    for (int i = 0; link.isUneven() && i < 420; ++i) link.observe(now += .05, 10);
    assert(!link.isUneven());
    std::cout << "PASS link classification enters after consecutive delays and recovers after a quiet window\n";
}

void heldLine() {
    bridge::PositionLink link;
    bridge::PositionFit fit;
    for (int i = 0; i < 20; ++i) {
        const double now = i * .05 + 1;
        assert(!link.observe(now, 10));
        assert(fit.add({"1", "PLAYING", 10000 + now * 1000, now, 10}));
    }
    for (int i = 0; i < 200; ++i) {
        const double now = 2 + i * .15;
        link.observe(now, 149);
        if (link.isUneven()) fit.retainQuickAnswers(link.trustedLimit());
        // Begin delayed traffic with two observations inside the old 200 ms
        // limit, then verify that the uneven link rejects the entire tail.
        if (i < 2) assert(!link.isUneven());
        else assert(link.isUneven() && link.trustedLimit() < 149);
        if (149 <= link.trustedLimit()) assert(fit.add({"1", "PLAYING", 10070 + now * 1000, now, 149}));
        assert(std::abs(fit.position(now)->positionMs - (10000 + now * 1000)) < .001);
    }
    assert(fit.add({"2", "TRANSITIONING", 3000, 33, 0, 33, true}));
    assert(fit.position(33.3)->positionMs == 3000);
    assert(fit.add({"2", "PLAYING", 3000, 33.4, 0, 33.4, true}));
    assert(std::abs(fit.position(33.7)->positionMs - 3300) < .001);
    std::cout << "PASS slow replies leave the last trusted line and state events restart it\n";
}
} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "paired") ownerPairing();
    else if (mode == "changes") recordedTransitions();
    else if (mode == "study") studyFit();
    else if (mode == "port") portFit();
    else if (mode == "classify") linkClassification();
    else if (mode == "hold") heldLine();
    else return 2;
}
