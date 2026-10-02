#include "source_ownership.h"
// sbstreamer.cpp -- HTTP request broker that serves the FLAC stream to Sonos
//
// Copyright (c) 2026 Jaap van Vliet
//
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
//
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

#include "upnp/stale_stream.h"
#include "sbstreamer.h"
#include "stream_session.h"
#include "upnp/timing.h"
#include "stream_close_log.h"

#include "sbencoder.h"
#include "stream_debug.h"
#include "audio_mode.h"
#include <map>
#include "sonos-position.h"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <mutex>
#include <unistd.h>

#define SBSTREAMER_ICON "/pulseaudio.png"
#define SBSTREAMER_CONTENT "audio/flac"
#define SBSTREAMER_DESC "Audio stream from %s"
#define SBSTREAMER_TIMEOUT 10000
// Four seconds waiting + bounded socket writes/EOF stays below ~five seconds.
#define SBSTREAMER_HTTP_IDLE_TIMEOUT 4000
// Allow the device play -> LMS CLI -> strm u round trip a full five seconds.
#define SBSTREAMER_RESUME_TIMEOUT 5000
#define SBSTREAMER_STANDBY_TIMEOUT 30000
#define SBSTREAMER_CHUNK 16384

using namespace bridge;

// The LMS generation outlives every HTTP connection. Each ACTIVE request owns
// a fresh FLAC encoder in that generation; STANDBY requests have no encoder.
static std::shared_ptr<SBEncoder> g_enc;
static std::mutex g_enc_mutex;
static std::map<unsigned, unsigned> streamRates;
// Publish the next format before its ID, without holding g_enc_mutex while
// new_squeezebox_stream_id takes the transport/resume locks.
extern "C" void set_squeezebox_audio_rate(unsigned stream, unsigned rate) {
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    streamRates[stream] = rate;
    while (streamRates.size() > 2) streamRates.erase(streamRates.begin());
}
// Ownership is independent of socket state. All request fields and slots are
// protected by g_enc_mutex; only ACTIVE requests have an encoder.
struct StreamRequest {
    std::chrono::steady_clock::time_point arrived = std::chrono::steady_clock::now();
    unsigned long long id;
    unsigned stream;
    std::string restartTag;
    std::shared_ptr<SBEncoder> encoder;
    std::atomic<bool> replaced{false};
    bool opened = false;
    bool pauseEnded = false;
    std::chrono::steady_clock::time_point expectedCloseAt{};
    std::atomic<bool> heldResume{false};
    std::atomic<bool> resumeAcknowledged{false};
    std::atomic<bool> serving{false};
};
static bridge::StreamCloseLog closeLog;
static std::atomic<bool> deferCloseLog{false};
extern "C" void configure_squeezebox_close_logging(bool own) { deferCloseLog = own; }
static unsigned long long nextRequestId = 0;
static unsigned ownershipStream = 0;
static std::shared_ptr<StreamRequest> activeRequest;
static std::shared_ptr<SBEncoder> canonicalEncoder;
static std::string canonicalRestartTag;
static std::vector<std::shared_ptr<StreamRequest>> standbyRequests;

// Caller holds g_enc_mutex, including when promoting a standby. Publish the
// fresh encoder and its owner together so PCM always goes to the ACTIVE request.
static void activateRequest(const std::shared_ptr<StreamRequest>& request, bool promoted)
{
    if (g_enc) g_enc->retireProducer();
    request->encoder = std::make_shared<SBEncoder>(request->stream);
    const auto rate = streamRates.find(request->stream);
    request->opened = request->encoder->open(audioMode() == AudioMode::Legacy ? 16 : 24,
        rate == streamRates.end() ? 44100 : rate->second);
    sonos_position_connection(request->stream, request->id);
    activeRequest = request;
    g_enc = request->encoder;
    canonicalEncoder = g_enc;
    canonicalRestartTag = request->restartTag;
    if (promoted && deferCloseLog) closeLog.observe(request->stream);
    if (promoted) request->expectedCloseAt = std::chrono::steady_clock::now();
    if (promoted)
        printf("stream %u: GET #%llu promoted\n", request->stream, request->id);
    printf("stream %u: GET #%llu ACTIVE\n", request->stream, request->id);
}
static unsigned endedByPause = 0;
extern void ResumeSqueezeBox(unsigned current);
extern void ResumeSqueezeBoxGetPair(unsigned, unsigned long long, unsigned long long,
                                  std::chrono::steady_clock::duration);
extern std::string SqueezeBoxURL(unsigned current);
extern "C" unsigned get_squeezebox_stream_id(void);
extern "C" unsigned get_lms_stream_serial(void);
extern "C" int yeney_is_paused(void);
extern "C" int sonos_output_running(void);

// Signal only the HTTP lifetime. Its worker sends EOF and retains the encoder
// until the next same-ID GET replaces it. No generation or FLAC close here.
static void endSqueezeboxResponse(bool flush)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    endedByPause = get_squeezebox_stream_id();
    if (deferCloseLog) closeLog.observe(endedByPause);
    if (activeRequest) activeRequest->expectedCloseAt = std::chrono::steady_clock::now();
    const bool preserve = flush && activeRequest && activeRequest->heldResume
        && !activeRequest->serving;
    if (preserve) activeRequest->resumeAcknowledged = false;
    if (!preserve && g_enc && g_enc->streamId() == endedByPause) g_enc->endResponse();
    // A pause ends existing requests, not a handoff to a standby. Standbys
    // have sent no headers and disconnect silently; later GETs may wait for PCM.
    for (auto& request : standbyRequests)
        request->pauseEnded = true;
    standbyRequests.clear();
}

extern "C" void note_squeezebox_device_close(void)
{
    if (deferCloseLog) closeLog.observe(get_squeezebox_stream_id());
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (activeRequest) activeRequest->expectedCloseAt = std::chrono::steady_clock::now();
}

extern "C" {
void end_squeezebox_response(void) { endSqueezeboxResponse(false); }
void flush_squeezebox_response(void) { endSqueezeboxResponse(true); }

void hold_squeezebox_resume(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (activeRequest && activeRequest->stream == stream && !activeRequest->encoder->hasAudio()
        && !activeRequest->encoder->responseEnded() && !activeRequest->encoder->cancelled())
        activeRequest->heldResume = true;
}

int squeezebox_response_streaming(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    return activeRequest && activeRequest->stream == stream && activeRequest->serving
        && g_enc && g_enc->hasAudio() && !g_enc->cancelled() && !g_enc->responseEnded();
}

int squeezebox_request_open(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (activeRequest && activeRequest->stream == stream) return 1;
    for (const auto& request : standbyRequests) if (request->stream == stream) return 1;
    return 0;
}

int squeezebox_response_open(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    return activeRequest && activeRequest->stream == stream && g_enc && g_enc->streamId() == stream && !g_enc->cancelled()
        && !g_enc->responseEnded();
}

int squeezebox_response_ended(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    return stream != 0 && endedByPause == stream;
}

void acknowledge_squeezebox_resume(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (endedByPause == stream) endedByPause = 0;
    if (activeRequest && activeRequest->stream == stream)
        activeRequest->resumeAcknowledged = true;
}

void invalidate_squeezebox_held_get(unsigned stream)
{
    std::lock_guard<std::mutex> lock(g_enc_mutex);
    if (g_enc && g_enc->streamId() == stream && !g_enc->hasAudio() && !g_enc->responseEnded()) {
        printf("stream %u: invalidating held GET for same-URL resume\n", stream);
        g_enc->cancel();
    }
}

int encode_squeezebox_audio_cancellable(const char* data, int len, uint64_t firstFrame, int (*cancel)(void*), void* context)
{
    auto cancelled = [=] { return cancel && cancel(context); };
    unsigned stream = get_squeezebox_stream_id();
    unsigned serial = get_lms_stream_serial();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!cancelled() && sonos_output_running() && stream == get_squeezebox_stream_id() && serial == get_lms_stream_serial()) {
        std::shared_ptr<SBEncoder> enc;
        uint64_t requestId = 0;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            enc = g_enc;
            if (activeRequest && enc == activeRequest->encoder) requestId = activeRequest->id;
        }
        if (enc && enc->streamId() == stream && !enc->cancelled() && !enc->responseEnded() && !enc->producerRetired()) {
            int written = enc->write(data, len, SBSTREAMER_TIMEOUT, [=] {
                sonos_position_pcm(stream, requestId, firstFrame);
            }, [=] { return cancelled() || !sonos_output_running(); });
            if (written == len) return 1;
            if (cancelled() || !sonos_output_running()) return 0;
            // Active-request termination or resume may replace the encoder
            // while write waits. Retry the SAME PCM block on the new encoder.
            if (!enc->cancelled() && !enc->responseEnded() && !enc->producerRetired()) {
                printf("encode_squeezebox_audio: write() failed %d != %d\n", written, len);
                return 0;
            }
        }
        if (yeney_is_paused())
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        if (std::chrono::steady_clock::now() >= deadline) {
            printf("encode_squeezebox_audio: timeout waiting for stream request\n");
            return 0;
        }
        usleep(1000);
    }
    return 0;
}
void encode_squeezebox_audio(const char* data, int len, uint64_t firstFrame) {
    encode_squeezebox_audio_cancellable(data, len, firstFrame, nullptr, nullptr);
}

} // extern "C"

void SBStreamer::registerWith(upnp::StreamServer& server)
{
    server.registerStream(SBSTREAMER_CNAME, SBSTREAMER_URI, SBSTREAMER_DESC,
        SBSTREAMER_CONTENT, SBSTREAMER_ICON,
        [this](upnp::StreamRequest& request) { return HandleRequest(&request); });
}

bool SBStreamer::HandleRequest(upnp::StreamRequest* handle)
{
    if (IsAborted() || handle->aborted())
        return false;

    const std::string& requestUri = handle->path();
    if (requestUri.compare(0, strlen(SBSTREAMER_URI), SBSTREAMER_URI) != 0)
        return false;

    const auto method = handle->method();
    if (method != upnp::StreamRequest::Method::Get && method != upnp::StreamRequest::Method::Head) return false;
    unsigned long long requestId = 0;
    if (method == upnp::StreamRequest::Method::Get) {
        { std::lock_guard<std::mutex> lock(g_enc_mutex); requestId = ++nextRequestId; }
        printf("stream %d: GET #%llu headers: %s\n", atoi(handle->parameter("stream").c_str()), requestId,
            upnp::streamHeaderLog(handle->headers()).c_str());
    }
    const std::string session = handle->parameter("session");
    if (session != streamSessionToken()) {
        printf("stale request: session %s != %s\n", session.c_str(), streamSessionToken().c_str());
        upnp::rejectStaleStream(*handle);
        return true;
    }

    switch (method) {
    case upnp::StreamRequest::Method::Get: {
        int stream = atoi(handle->parameter("stream").c_str());
        streamSqueezeBox(handle, stream, requestId);
        return true;
    }
    case upnp::StreamRequest::Method::Head: {
        handle->reply(200, SBSTREAMER_CONTENT);
        return true;
    }
    default:
        return false;  // unhandled method
    }
}

void SBStreamer::streamSqueezeBox(upnp::StreamRequest* handle, int stream, unsigned long long requestId)
{
    StreamDebugRequest diagnostics(*handle);
    handle = &diagnostics;
    printf("Sonos requested stream %d\n", stream);
    // Bound a stalled peer as well as a stalled PCM producer. Sending uses
    // the socket directly, so a receive timeout alone is insufficient.
    handle->sendTimeout(500);
    auto request = std::make_shared<StreamRequest>();
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        request->id = requestId;
        request->stream = stream;
        request->restartTag = handle->parameter("restart");
    }
    auto peerClosed = [handle, request] { return request->replaced.load() || handle->peerClosed(); };
    unsigned current = get_squeezebox_stream_id();
    if (stream <= 0 || (unsigned)stream > current) {
        handle->reply(400);
        return;
    }
    auto redirect = [&] {
        std::string url = SqueezeBoxURL(get_squeezebox_stream_id());
        if (!handle->header("Range").empty()) url += "&restart=" + std::to_string(requestId);
        std::string response = "HTTP/1.1 302 Found\r\nLocation: " + url
            + "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        printf("stream %d: HTTP 302 -> %s\n", stream, url.c_str());
        handle->send(response.c_str(), response.size());
    };
    if ((unsigned)stream < current) {
        redirect();
        return;
    }

    if (speakerRelinquished.load() && yeney_is_paused()) {
        // Sonos can restore the previous queue after AirPlay. Do not request
        // LMS resume and do not turn a paused queue into a 503 retry loop.
        const std::string paused = "HTTP/1.1 200 OK\r\nContent-Type: audio/flac\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        handle->send(paused.c_str(), paused.size());
        return;
    }

    const auto range = handle->header("Range");
    bool forceRestart;
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        forceRestart = !request->restartTag.empty() && request->restartTag != canonicalRestartTag;
        if (forceRestart && activeRequest) { activeRequest->replaced = true; activeRequest.reset(); }
    }
    if (!range.empty() && !forceRestart) {
        uint64_t offset = 0;
        std::shared_ptr<SBEncoder> canonical;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            canonical = canonicalEncoder;
        }
        auto restart = [&](const char* reason) {
            static std::mutex failureMutex;
            static auto last = std::chrono::steady_clock::time_point{};
            {
                std::lock_guard<std::mutex> lock(failureMutex);
                auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::seconds(5)) {
                    printf("stream %d: Range unavailable (%s) → restart\n", stream, reason); last = now;
                }
            }
            const auto url = SqueezeBoxURL(get_squeezebox_stream_id()) + "&restart=" + std::to_string(requestId);
            const auto response = "HTTP/1.1 302 Found\r\nLocation: " + url
                + "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
            handle->send(response.data(), response.size());
        };
        if (!openByteRange(range, offset) || !canonical || canonical->streamId() != (unsigned)stream
            || canonical->cancelled() || canonical->responseEnded() || canonical->producerRetired()) {
            restart("invalid range or inactive history"); return;
        }
        char bytes[SBSTREAMER_CHUNK];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        int count;
        while ((count = canonical->history.read(offset, bytes, sizeof(bytes))) == 0
               && !peerClosed() && !handle->aborted() && !IsAborted()
               && !canonical->historyFinished() && !canonical->responseEnded()
               && (unsigned)stream == get_squeezebox_stream_id()
               && std::chrono::steady_clock::now() < deadline) usleep(1000);
        if (peerClosed() || handle->aborted() || IsAborted()) return;
        if (count <= 0 || (unsigned)stream != get_squeezebox_stream_id()) {
            restart(count < 0 ? "evicted offset" : "offset not produced within 500 ms"); return;
        }
        bool superseded;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            superseded = canonical != canonicalEncoder || canonical->responseEnded() || canonical->cancelled()
                || (unsigned)stream != get_squeezebox_stream_id();
            if (!superseded) {
                if (activeRequest) activeRequest->replaced = true;
                request->encoder = canonical; request->opened = true; request->serving = true;
                activeRequest = request; g_enc = canonical; canonical->useHistoryReader();
            }
        }
        if (superseded) { restart("superseded history"); return; }
        diagnostics.encoder([canonical] { return canonical->encodedAudioMs(); }, true);
        const auto bounds = canonical->history.bounds();
        printf("stream %d: Range resume from %llu (history %llu–%llu) → continued\n", stream,
            (unsigned long long)offset, (unsigned long long)bounds.first, (unsigned long long)bounds.second);
        // Finite partial response, unknown total entity length. Never advertise
        // an open last-byte position or append bytes beyond the stated range.
        const uint64_t rangeEnd = bounds.second;
        const std::string headers = "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/flac\r\n"
            "Accept-Ranges: bytes\r\nContent-Range: bytes " + std::to_string(offset) + "-"
            + std::to_string(rangeEnd - 1) + "/*\r\nContent-Length: " + std::to_string(rangeEnd - offset)
            + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        bool sent = handle->send(headers.data(), headers.size());
        while (sent && offset < rangeEnd && !peerClosed() && !handle->aborted() && !IsAborted()
               && !canonical->cancelled() && !canonical->responseEnded() && !canonical->producerRetired()
               && (unsigned)stream == get_squeezebox_stream_id()) {
            count = canonical->history.read(offset, bytes, std::min<uint64_t>(sizeof(bytes), rangeEnd-offset));
            if (count <= 0) break; // slow receiver fell out of the bounded window
            sent = handle->send(bytes, count); offset += count;
        }
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            if (activeRequest == request) {
                activeRequest.reset();
                if (!yeney_is_paused() && (unsigned)stream == get_squeezebox_stream_id() && !standbyRequests.empty()) {
                    auto newest = std::max_element(standbyRequests.begin(), standbyRequests.end(),
                        [](const auto& a, const auto& b) { return a->id < b->id; });
                    auto promoted = *newest; standbyRequests.erase(newest); activateRequest(promoted, true);
                }
            }
        }
        handle->disconnect(); return;
    }
    // Recovery GET+Range pairs on firmware 86.10 send Connection: close.
    // Give that plain probe 100 ms to close before committing a new encoder.
    bool recoveryProbe = false;
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        recoveryProbe = canonicalEncoder && canonicalEncoder->hasAudio()
            && !canonicalEncoder->responseEnded() && !canonicalEncoder->cancelled()
            && canonicalEncoder->streamId() == (unsigned)stream && !yeney_is_paused()
            && handle->header("Connection") == "close";
    }
    if (recoveryProbe) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!peerClosed() && !handle->aborted() && !IsAborted()
               && std::chrono::steady_clock::now() < until) usleep(1000);
        if (peerClosed() || handle->aborted() || IsAborted()) return;
    }

    unsigned long long pairActive = 0;
    std::chrono::steady_clock::duration pairSeparation{};
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        // A track change may have won the race since request validation.
        if ((unsigned)stream == get_squeezebox_stream_id()) {
            if (ownershipStream != (unsigned)stream) {
                if (g_enc) g_enc->cancel();
                activeRequest.reset();
                standbyRequests.clear(); // old workers redirect on their next poll
                ownershipStream = stream;
            }
            // Preserve pause-ended encoders until a later GET replaces them.
            if (activeRequest && activeRequest->encoder->responseEnded())
                activeRequest.reset();
            if (!activeRequest) {
                activateRequest(request, false);
            } else {
                standbyRequests.push_back(request);
                pairSeparation = request->arrived - activeRequest->arrived;
                if (pairSeparation >= std::chrono::steady_clock::duration::zero()
                    && pairSeparation <= upnp::timing::kGetPairWindow)
                    pairActive = activeRequest->id;
                printf("stream %d: GET #%llu STANDBY\n", stream, request->id);
            }
        }
    }

    if (pairActive) ResumeSqueezeBoxGetPair(stream, pairActive, request->id, pairSeparation);

    std::shared_ptr<SBEncoder> enc;
    bool opened = false;
    auto standbyDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(SBSTREAMER_STANDBY_TIMEOUT);
    for (;;) {
        bool obsolete = false, disconnect = false;
        {
            std::lock_guard<std::mutex> lock(g_enc_mutex);
            obsolete = (unsigned)stream != get_squeezebox_stream_id();
            // An activated request must always take the active cleanup path,
            // even if its peer closes immediately after promotion.
            if (request->encoder) {
                enc = request->encoder;
                opened = request->opened;
                break;
            }
            if (peerClosed()) {
                printf("stream %d: GET #%llu standby closed by client\n", stream, request->id);
                disconnect = true;
            } else if (!obsolete && !request->pauseEnded && !(IsAborted() || handle->aborted())
                       && !yeney_is_paused() && activeRequest
                       && activeRequest->encoder->hasAudio()
                       && std::chrono::steady_clock::now() >= standbyDeadline) {
                printf("stream %d: GET #%llu standby timeout\n", stream, request->id);
                disconnect = true;
            }
            disconnect = disconnect || request->pauseEnded || (IsAborted() || handle->aborted());
            if (obsolete || disconnect)
                standbyRequests.erase(std::remove(standbyRequests.begin(), standbyRequests.end(), request),
                                      standbyRequests.end());
        }
        if (disconnect || obsolete) {
            if (obsolete && !disconnect) redirect();
            handle->disconnect();
            return;
        }
        usleep(5000);
    }

    // Only ACTIVE gets an encoder and headers. A held GET still means an
    // ACTIVE request waiting for PCM while LMS is paused, never a standby.
    bool waitForResumeAudio = yeney_is_paused() || request->heldResume;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SBSTREAMER_RESUME_TIMEOUT);
    auto waitingForAudio = [&] {
        return !enc->hasAudio() || (request->heldResume && !request->resumeAcknowledged);
    };
    while (opened && !(speakerRelinquished.load() && yeney_is_paused()) && waitForResumeAudio && waitingForAudio() && !enc->cancelled() && !enc->responseEnded() && !(IsAborted() || handle->aborted()) && !peerClosed()
           && (unsigned)stream == get_squeezebox_stream_id()
           && std::chrono::steady_clock::now() < deadline) {
        // A marked resume already sent LMS play. Its q/s reply may clear the
        // transport state, but must not renew the hold or send another play.
        if (!request->heldResume) ResumeSqueezeBox(stream);
        usleep(10000);
    }
    char buf[SBSTREAMER_CHUNK];
    int r = 0;
    const bool heldResume = request->heldResume;
    const bool closedResume = heldResume && peerClosed();
    const bool expiredResume = heldResume && waitingForAudio()
        && std::chrono::steady_clock::now() >= deadline;
    if (opened && !(speakerRelinquished.load() && yeney_is_paused()) && !closedResume && (!waitForResumeAudio || !waitingForAudio()) && !enc->cancelled())
        r = enc->read(buf, sizeof(buf), SBSTREAMER_HTTP_IDLE_TIMEOUT, false, peerClosed);
    bool streamReady = r >= 4 && memcmp(buf, "fLaC", 4) == 0;
    const std::string streamingHeaders = "HTTP/1.1 200 OK\r\nServer: " + handle->serverName() + "\r\nConnection: close\r\n"
        "Content-Type: audio/flac\r\nTransfer-Encoding: chunked\r\n\r\n";
    if (speakerRelinquished.load() && yeney_is_paused()) {
        const std::string paused = "HTTP/1.1 200 OK\r\nContent-Type: audio/flac\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        handle->send(paused.c_str(), paused.size());
    } else if (closedResume) {
        printf("held resume GET #%llu closed by client\n", request->id);
    } else if (!streamReady && waitForResumeAudio && (unsigned)stream != get_squeezebox_stream_id()) {
        if (heldResume)
            printf("held resume GET #%llu -> 302 stream %u\n", request->id, get_squeezebox_stream_id());
        // LMS play after stop can start a new delivery generation. The held
        // GET follows that URL instead of reporting an audio failure.
        redirect();
    } else if (!streamReady) {
        if (expiredResume) printf("held resume GET #%llu expired\n", request->id);
        printf("stream %d: no audio before timeout or connection replaced\n", stream);
        std::string error = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        handle->send(error.c_str(), error.size());
    } else {
        request->serving = true;
        if (heldResume) printf("held resume GET #%llu fed\n", request->id);
        printf("stream %d: serving current generation with fresh FLAC header\n", stream);
        diagnostics.encoder([enc] { return enc->encodedAudioMs(); });
        const auto responseStarted = std::chrono::steady_clock::now();
        size_t sent = 0;
        bool sendFailed = false;
        auto send = [&](const char* data, size_t size) {
            if (sendFailed) return false;
            errno = 0;
            if (handle->send(data, size)) {
                sent += size;
                return true;
            }
            const int error = errno; // capture before logging or cleanup changes it
            sendFailed = true;
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - responseStarted).count();
            if (deferCloseLog && (error == EPIPE || error == ECONNRESET)) {
                closeLog.failed(stream, seconds, sent, error);
                return false;
            }
            bool expected = false;
            {
                std::lock_guard<std::mutex> lock(g_enc_mutex);
                expected = (error == EPIPE || error == ECONNRESET)
                    && request->expectedCloseAt != std::chrono::steady_clock::time_point{}
                    && std::chrono::steady_clock::now() - request->expectedCloseAt < std::chrono::seconds(2);
            }
            if (expected)
                printf("stream %d: client closed after %.1f s (%zu bytes confirmed sent; %s; errno=%d)\n",
                    stream, seconds, sent, strerror(error), error);
            else printf("stream %d: send to Sonos failed after %.1f s (%s; errno=%d; "
                   "%zu bytes confirmed sent, partial failed write uncounted)\n",
                stream, seconds, error ? strerror(error) : "short write or closed socket",
                error, sent);
            return false;
        };
        auto sendChunk = [&](const char* data, size_t size) {
            char header[16];
            int length = snprintf(header, sizeof(header), "%x\r\n", (unsigned)size);
            return send(header, length) && send(data, size) && send("\r\n", 2);
        };
        if (send(streamingHeaders.c_str(), streamingHeaders.size()) && sendChunk(buf, r)) {
            while (!(IsAborted() || handle->aborted()) && (r = enc->read(buf, sizeof(buf), SBSTREAMER_HTTP_IDLE_TIMEOUT, false, peerClosed)) > 0) {
                if (!sendChunk(buf, r)) break;
            }
            send("0\r\n\r\n", 5);
        }
    }
    if (peerClosed()) printf("stream %d: client closed connection\n", stream);
    {
        std::lock_guard<std::mutex> lock(g_enc_mutex);
        const bool pauseEnded = enc->responseEnded();
        const bool retain = enc == canonicalEncoder && enc->hasAudio() && !yeney_is_paused()
            && !enc->producerRetired() && (unsigned)stream == get_squeezebox_stream_id();
        if (!pauseEnded && !retain) enc->cancel();
        if (activeRequest == request) {
            activeRequest.reset();
            if (!pauseEnded) {
                if (g_enc == enc && !retain) g_enc.reset();
                if ((unsigned)stream == get_squeezebox_stream_id() && !standbyRequests.empty()) {
                    auto newest = std::max_element(standbyRequests.begin(), standbyRequests.end(),
                        [](const std::shared_ptr<StreamRequest>& a, const std::shared_ptr<StreamRequest>& b) {
                            return a->id < b->id;
                        });
                    auto promoted = *newest;
                    standbyRequests.erase(newest);
                    activateRequest(promoted, true);
                }
            }
        }
    }
    // Preserve paused encoders and send EOF before disconnecting.
    handle->disconnect();
    if (enc->cancelled() && !enc->responseEnded()) enc->close();
    printf("stream %d: done\n", stream);

    printf("Done serving stream %d to Sonos\n", stream);
}
