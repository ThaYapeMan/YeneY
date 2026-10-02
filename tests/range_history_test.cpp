#include "encoded_history.h"
#include <cassert>
#include <iostream>
int main() {
    EncodedHistory history(8);
    char bytes[32]; uint64_t offset;
    assert(openByteRange("bytes=622592-", offset) && offset == 622592);
    for (auto bad : {"bytes=-1", "bytes=1-2", "bytes=1-,2-", "bytes=18446744073709551616-", "bytes=-"})
        assert(!openByteRange(bad, offset));
    history.append("fLaC1234",8);
    assert(history.read(0,bytes,8)==8 && std::string(bytes,8)=="fLaC1234");
    assert(history.read(8,bytes,8)==0 && history.read(10,bytes,8)==0);
    history.append("5678",4);
    assert(history.read(0,bytes,8)==-1);
    assert(history.read(4,bytes,8)==8 && std::string(bytes,8)=="12345678");
    history.append("abcdefghijk",11);
    assert(history.read(15,bytes,8)==8 && std::string(bytes,8)=="defghijk");
    std::cout << "PASS: history start/end/future/eviction, wrap/oversize append and strict Range parsing\n";
}
