// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lms_icon.h"
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    uint8_t mac[]{0x94, 0x9f, 0x3e, 0xfa, 0xba, 0x66};
    bridge::LmsIcon icon(mac, [](const auto &s) { std::cout << s << std::endl; }, std::stoul(argv[1]));
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit")
            break;
        icon.connected("127.0.0.1");
    }
}
