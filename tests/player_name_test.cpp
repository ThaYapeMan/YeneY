// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "player_name.h"
int main(int argc, char **argv) {
    auto name = bridge::playerNames("Study", argc > 2 ? argv[2] : nullptr, argv[1]);
    return name.trueName == "Study (Sonos)" ? 0 : 1;
}
