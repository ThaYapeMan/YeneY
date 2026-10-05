OWN_UPNP_SOURCES = upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/http.cpp upnp/soap.cpp upnp/discovery.cpp upnp/own_speaker_control.cpp
UPNP_OBJS = $(OWN_UPNP_SOURCES:.cpp=.o) upnp/encoded_buffer.o

OBJS = core_sonos.o core_shm.o audio_mode.o $(UPNP_OBJS) yeney.o sbstreamer.o sbencoder.o sonos-status.o sonos-position.o

all: yeney

CORE_LIB = third_party/yeney-core/libyeneycore.a
.PHONY: core-library
core-library:
$(CORE_LIB): core-library
	$(MAKE) -C third_party/yeney-core libyeneycore.a

core_sonos.o: core_sonos.h audio_mode.h sonos-position.h third_party/yeney-core/core/player.h third_party/yeney-core/core/sink.h
yeney.o: obsolete_player.h
core_sonos.o: %.o: %.cpp
	g++ -std=c++17 -g -O3 -Wall -Wextra -Ithird_party/yeney-core -c -o $@ $<

yeney: $(OBJS) $(CORE_LIB)
	g++ -g -o $@ $^ \
		-lFLAC++ -lFLAC -lcrypto \
		-lpthread

clean:
	rm -f cold-position-test lms-position-test timing-tap-test timing-contract-test timing-probe-field-test timing-probe-settings-test timing-probe-test timing-probe-resets-test timing-probe-http-test lead-test range-test range-history-test encoded-buffer-test stream-content-test http-server-test speaker-state-test *.o tests/*.o upnp/*.o yeney position-test encoder-test resume-state-test streamer-test upnp-test own-control-test

.PHONY: test install
install: yeney
	install -d -m 0755 /var/lib/yeney
	scripts/install-devices.sh
encoder-test: tests/audio_pack_fixture.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h tests/encoder_test.cpp sbencoder.cpp sbencoder.h
	g++ -g -O2 -Wall -I. -DSBENCODER_TEST -o $@ tests/encoder_test.cpp tests/audio_pack_fixture.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread

test: export YENEY_START_LEAD_MS = 0
test: export YENEY_TIMING_PROBE = 0
test: export YENEY_TIMING_RAW = 0
test: export YENEY_TIMING_PUBLISH = 0
test: export YENEY_TIMING_LOCKED_EVERY_S = 5
test: export YENEY_AUDIBLE_OFFSET_MS = 0
test: export YENEY_TIMING_STALE_S = 60
test: timing-probe-field-test timing-probe-settings-test timing-probe-test timing-probe-resets-test timing-probe-http-test lead-test range-history-test range-test encoded-buffer-test stream-content-test http-server-test speaker-state-test yeney position-test encoder-test resume-state-test streamer-test upnp-test own-control-test
	YENEY_TIMING_PROBE=1 YENEY_TIMING_PUBLISH=1 ./timing-tap-test
	./timing-contract-test
	./cold-position-test
	./lms-position-test
	python3 tests/lms_position_check_test.py
	python3 tests/timing_contract_test.py
	./timing-probe-test
	./timing-probe-field-test
	python3 tests/timing_probe_settings_test.py
	YENEY_TIMING_STALE_S=10 ./timing-probe-resets-test
	python3 tests/timing_probe_http_test.py
	python3 tests/timing_probe_raw_http_test.py
	python3 tests/stream_debug_test.py
	python3 tests/lead_setting_test.py
	YENEY_START_LEAD_MS=2000 ./lead-test
	YENEY_START_LEAD_MS=0 ./lead-test
	YENEY_START_LEAD_MS=2000 ./range-test
	python3 tests/start_lead_test.py
	./range-history-test
	./range-test
	./encoded-buffer-test
	python3 tests/encoded_overwrite_test.py
	python3 tests/http_server_test.py
	./speaker-state-test
	./upnp-test
	./stream-content-test
	python3 tests/title_format_setting_test.py
	python3 tests/stream_content_setting_test.py
	python3 tests/list_rooms_test.py
	python3 tests/installer_test.py
	python3 tests/migration_test.py
	python3 tests/upnp_mock_test.py
	python3 tests/own_display_test.py
	./position-test
	python3 tests/position_reconnect_test.py
	python3 tests/send_error_test.py
	./streamer-test session
	./streamer-test position
	./streamer-test shutdown
	./encoder-test
	./resume-state-test
	YENEY_PAUSE=pause ./streamer-test
	env -u YENEY_PAUSE ./streamer-test stop
	python3 tests/http_streamer_test.py
	python3 tests/device_resume_test.py
	python3 tests/yeney_timeline_test.py
	python3 tests/lms_discovery_test.py
	python3 tests/device_test_script_test.py
	python3 tests/replaygain_probe_test.py
	python3 tests/pause_mode_test.py
	python3 tests/audio_mode_test.py
	python3 tests/obsolete_player_test.py
	python3 tests/core_sonos_test.py
	python3 tests/player_engine_test.py
	$(MAKE) -C third_party/yeney-core test

sbstreamer.o sbencoder.o: sbencoder.h

yeney.o: legacy_environment.h resume_state.h stop_debounce.h
resume-state-test: tests/resume_state_test.cpp resume_state.h stop_debounce.h
	g++ -g -O2 -Wall -I. -o $@ tests/resume_state_test.cpp

streamer-test: upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h pause_mode.h tests/streamer_test.cpp sbstreamer.cpp sbstreamer.h sbencoder.cpp sbencoder.h resume_state.h
	g++ -g -O2 -Wall -I. -o $@ tests/streamer_test.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread

yeney.o streamer-test: pause_mode.h

position-test: tests/position_test.cpp position_state.h
	g++ -g -O2 -Wall -I. -o $@ tests/position_test.cpp
sonos-position.o: position_state.h sonos-position.h
yeney.o sbstreamer.o streamer-test: sonos-position.h
streamer-test: sonos-position.cpp position_state.h

yeney.o: transport_intent.h retry_budget.h

yeney.o sbstreamer.o streamer-test: stream_session.h

$(OBJS) streamer-test: upnp/speaker_control.h upnp/stream_server.h

$(UPNP_OBJS) yeney.o: $(wildcard upnp/*.h)

upnp-test: $(wildcard upnp/*.h) tests/upnp_test.cpp upnp/xml.cpp upnp/soap.cpp upnp/http.cpp upnp/discovery.cpp
	g++ -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^)

own-control-test: $(wildcard upnp/*.h) tests/own_control_fixture.cpp $(OWN_UPNP_SOURCES)
	g++ -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread

upnp/encoded_buffer.o sbencoder.o: upnp/encoded_buffer.h

yeney.o: upnp/list_rooms.h

sbstreamer.o streamer-test: stream_close_log.h upnp/stale_stream.h

yeney.o sonos-status.o: sonos-status.h
sonos-status.o: speaker_uri.h stream_session.h

audio_mode.o yeney.o sbstreamer.o streamer-test: audio_mode.h

speaker-state-test: tests/speaker_state_test.cpp $(wildcard upnp/*.h) upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/speaker_state_test.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp

yeney.o sbstreamer.o streamer-test resume-state-test: upnp/timing.h

http-server-test: $(wildcard upnp/*.h) tests/http_server_fixture.cpp upnp/http_server.cpp upnp/http_server.h upnp/stream_server.h upnp/timing.h
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/http_server_fixture.cpp upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp -lpthread

http-server-test: upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/icon.h

stream-content-test: tests/stream_content_test.cpp upnp/soap.cpp upnp/xml.cpp $(wildcard upnp/*.h)
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/stream_content_test.cpp upnp/soap.cpp upnp/xml.cpp

encoded-buffer-test: tests/encoded_buffer_test.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/encoded_buffer_test.cpp upnp/encoded_buffer.cpp -lpthread

upnp/http_server.o streamer-test own-control-test: upnp/icon.h

yeney.o sbstreamer.o streamer-test: source_ownership.h speaker_uri.h

sbencoder.o sbstreamer.o encoder-test streamer-test: encoded_history.h sbencoder.h start_lead.h stream_debug.h

range-history-test: tests/range_history_test.cpp encoded_history.h sbencoder.h start_lead.h stream_debug.h
	g++ -std=c++17 -O2 -Wall -I. -o $@ $<
range-test: tests/range_fixture.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp upnp/encoded_buffer.cpp $(wildcard upnp/*.h) encoded_history.h sbencoder.h start_lead.h stream_debug.h
	g++ -std=c++17 -O2 -Wall -I. -o $@ $(filter %.cpp,$^) -lFLAC++ -lFLAC -lcrypto -lpthread

lead-test: tests/lead_fixture.cpp sbencoder.cpp sbencoder.h start_lead.h upnp/encoded_buffer.cpp
	g++ -g -O2 -Wall -I. -o $@ tests/lead_fixture.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread

sbencoder.o core_sonos.o: start_lead.h
sbstreamer.o: stream_debug.h sbencoder.h
upnp/http_server.o: upnp/http_server.h upnp/stream_server.h

sonos-position.o: position_state.h start_lead.h

core_sonos.o encoder-test: pcm_pack.h

# Measurement-only timing probe fixtures.
timing-probe-test: tests/timing_probe_test.cpp timing_probe.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $<
timing-probe-resets-test: tests/timing_probe_resets_test.cpp sonos-position.cpp timing_probe.h timing_probe_runtime.h position_state.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread
timing-probe-http-test: tests/timing_probe_http_fixture.cpp $(OWN_UPNP_SOURCES) timing_probe.h timing_probe_runtime.h $(wildcard upnp/*.h)
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread
core_sonos.o sonos-position.o sbstreamer.o upnp/own_speaker_control.o upnp/http.o: timing_probe.h timing_probe_runtime.h

timing-probe-field-test: tests/timing_probe_field_test.cpp timing_probe.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $<
timing-probe-settings-test: tests/timing_probe_settings_fixture.cpp timing_probe.h timing_probe_runtime.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $< -lpthread

core_sonos.o: timing_tap.h timing_settings.h timing_channel.h timing_brackets.h
core_sonos.o sonos-position.o sbstreamer.o upnp/own_speaker_control.o upnp/http.o: timing_settings.h timing_channel.h timing_brackets.h

timing-contract-test: tests/timing_contract_test.cpp timing_probe_runtime.h timing_probe.h timing_brackets.h timing_channel.h timing_settings.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<
test: timing-contract-test

timing-tap-test: tests/timing_tap_test.cpp timing_tap.h timing_probe_runtime.h timing_brackets.h timing_channel.h timing_settings.h core_shm.o $(CORE_LIB)
	g++ -std=c++17 -O2 -Wall -Wextra -I. -Ithird_party/yeney-core -pthread -o $@ $< core_shm.o $(CORE_LIB) -lFLAC
test: timing-tap-test

core_shm.o: third_party/yeney-core/sinks/shm_v1/sink.cpp third_party/yeney-core/core/sink.h third_party/yeney-core/sinks/shm_v1/sink.h third_party/yeney-core/sinks/shm_v1/layout.h
	g++ -std=c++17 -O2 -Wall -Wextra -Ithird_party/yeney-core -c -o $@ $<

timing-probe-settings-test timing-probe-resets-test timing-probe-http-test streamer-test range-test own-control-test: timing_brackets.h timing_channel.h timing_settings.h

lms-position-test: tests/lms_position_test.cpp position_state.h timing_probe_runtime.h timing_settings.h timing_brackets.h timing_channel.h timing_probe.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<
test: lms-position-test
sonos-position.o position-test: position_state.h

core_sonos.o sonos-position.o sbstreamer.o upnp/own_speaker_control.o timing-contract-test lms-position-test: timing_drift_state.h

cold-position-test: tests/cold_position_test.cpp timing_brackets.h timing_probe.h timing_drift_state.h timing_probe_runtime.h timing_settings.h timing_channel.h position_state.h
	g++ -std=c++17 -g -O2 -Wall -I. -o $@ $<
test: cold-position-test
