#include "sbstreamer.h"
#include "start_lead.h"
#include "stream_session.h"
#include "source_ownership.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <FLAC++/decoder.h>
static std::atomic<unsigned> stream{3}, serial{1}, plays{0};
static std::atomic<bool> paused{false};
extern "C" unsigned get_squeezebox_stream_id() { return stream; }
extern "C" unsigned get_lms_stream_serial() { return serial; }
extern "C" int yeney_is_paused() { return paused; }
extern "C" int sonos_output_running() { return 1; }
extern "C" uint64_t get_sb_time_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
extern "C" void encode_squeezebox_audio(const char*,int,uint64_t);
extern "C" void end_squeezebox_response();
void ResumeSqueezeBox(unsigned) { ++plays; }
void ResumeSqueezeBoxGetPair(unsigned,unsigned long long,unsigned long long,std::chrono::steady_clock::duration) { ++plays; }
std::string SqueezeBoxURL(unsigned id) { return "http://bridge/music/yeney.flac?session="+streamSessionToken()+"&stream="+std::to_string(id); }
struct Request : upnp::StreamRequest {
    int phase = 0;
    uint64_t offset = 0;
    bool range = false;
    std::string restartTag;
    unsigned id = 3;
    std::atomic<bool> closed{false}, headersSent{false};
    std::vector<char> body;
    std::string response;
    std::string path() const override { return "/music/yeney.flac"; }
    Method method() const override { return Method::Get; }
    std::string parameter(const std::string& key) const override { return key=="session" ? streamSessionToken() : key=="stream" ? std::to_string(id) : key=="restart" ? restartTag : ""; }
    upnp::RequestHeaders headers() const override { return range ? upnp::RequestHeaders{{"RANGE","bytes="+std::to_string(offset)+"-"},{"CONNECTION","close"}} : upnp::RequestHeaders{{"CONNECTION","close"}}; }
    bool send(const char* data,size_t n) override {
        if (closed) return false;
        if (!headersSent) { response.assign(data,n); headersSent = true; }
        else if (response.find("206 Partial Content")!=std::string::npos) body.insert(body.end(),data,data+n);
        else if (!(n==5 && !memcmp(data,"0\r\n\r\n",5))) {
            if (phase==1) body.insert(body.end(),data,data+n);
            phase=(phase+1)%3;
        }
        return true;
    }
    bool peerClosed() override { return closed; }
    void sendTimeout(unsigned) override {}
    void disconnect() override {}
    void reply(unsigned status,const std::string& = {}) override { response=std::to_string(status); headersSent=true; }
    std::string serverName() const override { return "fixture"; }
};
template<class F> void wait(F test) {
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!test()) { assert(std::chrono::steady_clock::now()<end); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
class Decoder : public FLAC::Decoder::Stream {
public:
    const std::vector<char>& data; size_t at=0; uint64_t frames=0;
    explicit Decoder(const std::vector<char>& data):data(data) {}
    FLAC__StreamDecoderReadStatus read_callback(FLAC__byte* buffer,size_t* n) override {
        *n=std::min(*n,data.size()-at); memcpy(buffer,data.data()+at,*n); at+=*n;
        return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    FLAC__StreamDecoderWriteStatus write_callback(const FLAC__Frame* frame,const FLAC__int32* const[]) override {
        assert(frame->header.bits_per_sample==24 && frame->header.sample_rate==44100);
        frames+=frame->header.blocksize; return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    void error_callback(FLAC__StreamDecoderErrorStatus) override { assert(false); }
};
int main() {
    bridge::SBStreamer broker;
    Request original;
    auto first=std::async(std::launch::async,[&]{ broker.HandleRequest(&original); });
    wait([&]{return original.headersSent.load();});
    // Dense, deterministic incompressible PCM exposes frame/burst differences.
    std::vector<char> pcm(8192*6); uint32_t rng=17;
    for(auto& b:pcm) { rng=rng*1664525u+1013904223u; b=rng>>24; }
    auto producer=std::async(std::launch::async,[&]{for(int i=0;i<24;++i) { if(i==12) for(auto& b:pcm) b=~b; encode_squeezebox_audio(pcm.data(),pcm.size(),i*8192); }});
    std::this_thread::sleep_for(std::chrono::milliseconds(yeney_start_lead_ms() ? 500 : 2200));
    original.closed=true; first.get();
    auto canonical=original.body;
    assert(canonical.size()>100000 && !memcmp(canonical.data(),"fLaC",4));
    uint64_t offset=canonical.size()-4096;
    Request probe;
    auto probing=std::async(std::launch::async,[&]{broker.HandleRequest(&probe);});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    probe.closed=true; probing.get();
    assert(!probe.headersSent);
    Request resume; resume.range=true; resume.offset=offset;
    auto recovered=std::async(std::launch::async,[&]{broker.HandleRequest(&resume);});
    wait([&]{return resume.headersSent.load();});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    resume.closed=true; recovered.get();
    assert(resume.response.find("206 Partial Content")!=std::string::npos);
    assert(resume.response.find("Content-Range: bytes "+std::to_string(offset)+"-")!=std::string::npos);
    assert(resume.response.find("Content-Length: "+std::to_string(resume.body.size())+"\r\n")!=std::string::npos);
    assert(resume.body.size()>=4096 && memcmp(resume.body.data(),"fLaC",4));
    assert(!memcmp(resume.body.data(),canonical.data()+offset,4096));
    // Same stream id models a gapless PCM boundary, not a new HTTP entity.
    auto continued=canonical; continued.resize(offset); continued.insert(continued.end(),resume.body.begin(),resume.body.end());
    Request probe2; probe2.closed=true; broker.HandleRequest(&probe2);
    Request resume2; resume2.range=true; resume2.offset=offset+resume.body.size();
    auto second=std::async(std::launch::async,[&]{broker.HandleRequest(&resume2);});
    wait([&]{return resume2.headersSent.load();});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    resume2.closed=true; second.get();
    assert(resume2.body.size()>0 && memcmp(resume2.body.data(),"fLaC",4));
    Request ahead; ahead.range=true; ahead.offset=resume2.offset+resume2.body.size()+1024;
    auto futureData=std::async(std::launch::async,[&]{broker.HandleRequest(&ahead);});
    wait([&]{return ahead.headersSent.load();});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ahead.closed=true; futureData.get();
    assert(ahead.response.find("206 Partial Content")!=std::string::npos && !ahead.body.empty());
    producer.get();
    encode_squeezebox_audio(nullptr,0,0); // flush the final partial frame to history
    Request entire; entire.range=true; entire.offset=0; broker.HandleRequest(&entire);
    assert(!memcmp(entire.body.data(),"fLaC",4));
    Decoder decoder(entire.body);
    assert(decoder.init()==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    assert(decoder.process_until_end_of_stream());
    assert(decoder.frames==24*8192);
    assert(!memcmp(entire.body.data()+offset,resume.body.data(),resume.body.size()));
    assert(!memcmp(entire.body.data()+resume2.offset,resume2.body.data(),resume2.body.size()));
    Request future; future.range=true; future.offset=UINT64_MAX;
    broker.HandleRequest(&future);
    assert(future.response.find("302 Found")!=std::string::npos && future.response.find("restart=")!=std::string::npos);
    Request stale; stale.range=true; stale.id=2; broker.HandleRequest(&stale);
    assert(stale.response.find("302 Found")!=std::string::npos && stale.response.find("restart=")!=std::string::npos);
    Request fresh; fresh.range=true; fresh.offset=UINT64_MAX;
    auto at=future.response.find("&restart=")+9;
    fresh.restartTag=future.response.substr(at,future.response.find("\r\n",at)-at);
    auto restarted=std::async(std::launch::async,[&]{broker.HandleRequest(&fresh);});
    wait([&]{return fresh.headersSent.load();});
    encode_squeezebox_audio(pcm.data(),pcm.size(),24*8192);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fresh.closed=true; restarted.get();
    assert(fresh.response.find("200 OK")!=std::string::npos && !memcmp(fresh.body.data(),"fLaC",4));
    Request afterRestart; afterRestart.range=true; afterRestart.restartTag=fresh.restartTag;
    afterRestart.offset=fresh.body.size()-128;
    broker.HandleRequest(&afterRestart);
    assert(afterRestart.response.find("206 Partial Content")!=std::string::npos);
    assert(!memcmp(afterRestart.body.data(),fresh.body.data()+afterRestart.offset,128));
    assert(plays==0 && !paused);
    end_squeezebox_response();
    std::cout<<"PASS: repeated GET/close/Range recovery retains byte identity across PCM boundaries, no FLAC header reset or resume relay; future/stale ranges restart\n";
}
