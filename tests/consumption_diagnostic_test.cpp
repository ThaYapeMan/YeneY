// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "consumption_diagnostic.h"
#include <atomic>
#include "upnp/tcp_backlog.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <cassert>
#include <unistd.h>
#include <fstream>
#include <sstream>
struct FakeSonos : upnp::StreamRequest {
    std::atomic<uint64_t> served{12000};
    std::atomic<int64_t> pending{2000};
    std::string path() const override { return "/yeney/q/SECRET/token.wav"; }
    Method method() const override { return Method::Get; }
    std::string parameter(const std::string&) const override { return "SECRET"; }
    uint64_t connectionSentBytes() const override { return served; }
    int64_t connectionBacklog() const override { return pending; }
    bool send(const char*,size_t) override { return true; }
    bool peerClosed() override { return false; }
    void sendTimeout(unsigned) override {}
    void disconnect() override {}
    void reply(unsigned,const std::string&) override {}
    std::string serverName() const override { return "fake"; }
};
int main() {
    using namespace bridge;
    // Actual TCP loopback peer: stop reading until the send queue fills.
    int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    socklen_t length=sizeof(address); assert(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0);
    assert(listen(listener,1)==0);
    int client=socket(AF_INET,SOCK_STREAM,0);assert(client>=0);
    assert(connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    int peer=accept(listener,nullptr,nullptr);assert(peer>=0);
    char block[65536]{}; uint64_t accepted=0;
    for (;;) { auto n=send(client,block,sizeof(block),MSG_DONTWAIT|MSG_NOSIGNAL);if(n<=0)break;accepted+=n; }
    auto backlog=upnp::tcpBacklog(client);assert(backlog>0 && uint64_t(backlog)<=accepted);
    assert(upnp::tcpBacklog(-1)==-1);
    close(peer);close(client);close(listener);

    auto line = consumptionLine(1000,12000,2000,44100,true,999,"5374756479","queue");
    assert(line.find("served=12000 backlog=2000 audible=10000 content_frame=44100") != std::string::npos);
    assert(consumptionLine(1,10,20,0,false,0,"","queue").find("audible=0 content_frame=unknown") != std::string::npos);
    assert(consumptionLine(1,10,-1,0,false,0,"","stream").find("backlog=unknown audible=unknown content_frame=unknown") != std::string::npos);
    assert(consumptionLine(1,UINT64_MAX,0,0,false,0,"","stream").find("served=unknown") != std::string::npos);
    // Capture stdout without a disk temporary file. The fake peer controls the
    // accepted socket offset and queued bytes independently of the producer.
    int fds[2]; assert(pipe(fds)==0); fflush(stdout);
    const int saved = dup(STDOUT_FILENO); assert(dup2(fds[1],STDOUT_FILENO)>=0); close(fds[1]);
    FakeSonos socket;
    consumptionContent().set(44100,true);
    {
        ConsumptionDiagnostic diagnostic(socket,"Study","queue");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        socket.served = 15000; socket.pending = -1;
        consumptionContent().set(0,false); // TIMING-01 MP3/unsupported provenance
        std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    }
    fflush(stdout); assert(dup2(saved,STDOUT_FILENO)>=0); close(saved);
    char bytes[4096]; auto n=read(fds[0],bytes,sizeof(bytes));close(fds[0]);assert(n>0);
    std::string output(bytes,n); assert(output.find("SECRET")==std::string::npos);
    assert(output.find("content_frame=44100")!=std::string::npos);
    assert(output.find("backlog=unknown audible=unknown content_frame=unknown")!=std::string::npos);
    std::istringstream lines(output); unsigned count=0;uint64_t previous=0;
    while(std::getline(lines,line)) {
        const auto at=line.find("native_ms=");assert(at!=std::string::npos);
        const auto now=std::stoull(line.substr(at+10));
        if(count) assert(now-previous>=800 && now-previous<=1200);
        previous=now;++count;
    }
    assert(count==3);
    puts("PASS consumption diagnostic: controlled socket accounting, 1 Hz cadence, known/unsupported content, unknown backlog, masking");
}
