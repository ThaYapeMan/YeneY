OWN_UPNP_SOURCES = upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/http.cpp upnp/soap.cpp upnp/discovery.cpp upnp/own_speaker_control.cpp
UPNP_OBJS = $(OWN_UPNP_SOURCES:.cpp=.o) upnp/encoded_buffer.o

QUEUE_ROOM_OBJS = queue_room.o queue_relay.o queue_path.o queue_speaker.o queue_lms_commands.o queue_status.o

OBJS = $(QUEUE_ROOM_OBJS) core_queue_sink.o core_sonos.o core_shm.o audio_mode.o $(UPNP_OBJS) yeney.o sbstreamer.o sbencoder.o sonos-status.o sonos-position.o

all: yeney libqueueitems.a yeney-queue-dryrun

CORE_LIB = third_party/yeney-core/libyeneycore.a
.PHONY: core-library
core-library:
$(CORE_LIB): core-library
	$(MAKE) -C third_party/yeney-core libyeneycore.a

core_sonos.o: pcm_pack.h core_sonos.h audio_mode.h sonos-position.h third_party/yeney-core/core/player.h third_party/yeney-core/core/sink.h
yeney.o: obsolete_player.h playback_mode.h
core_sonos.o: %.o: %.cpp
	g++ -std=c++17 -g -O3 -Wall -Wextra -Ithird_party/yeney-core -c -o $@ $<

yeney: $(OBJS) libqueuecontroller.a libqueueitems.a $(CORE_LIB)
	g++ -g -o $@ $^ \
		-lFLAC++ -lFLAC -lcrypto \
		-lpthread

clean:
	rm -f lms-icon-test lms-icon-connection-test queue-source-test room-status-test xml-sanitize-test stream-metrics-lifetime-test stream-pacing-encoder-test encoded-backpressure-test stream-pacing-test stream-metrics-test queue-position-test libqueuecontroller.a playback-mode-test queue-ownership-test queue-actions-test queue-metadata-test queue-route-test yeney-queue-dryrun core-queue-sink-test libqueueitems.a queue-item-store-test oct12-boundary-test oct11-acquisition-test field-cold-test cold-position-test lms-position-test timing-tap-test timing-contract-test timing-probe-field-test timing-probe-settings-test timing-probe-test timing-probe-resets-test timing-probe-http-test lead-test range-test range-history-test encoded-buffer-test stream-content-test http-server-test speaker-state-test *.o tests/*.o upnp/*.o yeney position-test encoder-test resume-state-test streamer-test upnp-test own-control-test

.PHONY: test install
install: yeney yeney-queue-dryrun
	install -d -m 0755 /var/lib/yeney
	install -m 0755 yeney-queue-dryrun /usr/local/bin/yeney-queue-dryrun
	install -m 0755 scripts/yeney-queue-suite /usr/local/bin/yeney-queue-suite
	install -m 0644 scripts/yeney_room_state.py /usr/local/bin/yeney_room_state.py
	install -m 0755 scripts/yeney-stream-pacing-suite /usr/local/bin/yeney-stream-pacing-suite
	install -m 0755 scripts/yeney-queue-device-suite /usr/local/bin/yeney-queue-device-suite
	install -d -m 0755 /usr/local/share/yeney
	install -m 0644 assets/sonos.svg /usr/local/share/yeney/sonos.svg
	install -m 0755 scripts/yeney-install-lms-icon /usr/local/bin/yeney-install-lms-icon
	scripts/install-devices.sh $(if $(LMS_PREFS_DIR),--lms-prefs-dir="$(LMS_PREFS_DIR)")
encoder-test: tests/audio_pack_fixture.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h tests/encoder_test.cpp sbencoder.cpp sbencoder.h
	g++ -g -O2 -Wall -I. -DSBENCODER_TEST -o $@ tests/encoder_test.cpp tests/audio_pack_fixture.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread

# Preserve the legacy wire fixtures explicitly; settings tests also exercise the unset default.
test: export YENEY_DEBUG =
test: export YENEY_LMS_ICON =
test: export YENEY_STREAM_PACING = wall
test: export YENEY_PLAYBACK_MODE = stream
test: export YENEY_START_LEAD_MS = 0
test: export YENEY_TIMING_PROBE = 0
test: export YENEY_TIMING_RAW = 0
test: export YENEY_TIMING_PRIOR = 1
test: export YENEY_TIMING_PUBLISH = 0
test: export YENEY_TIMING_LOCKED_EVERY_S = 5
test: export YENEY_AUDIBLE_OFFSET_MS = 0
test: export YENEY_TIMING_STALE_S = 60
test: timing-probe-field-test timing-probe-settings-test timing-probe-test timing-probe-resets-test timing-probe-http-test lead-test range-history-test range-test encoded-buffer-test stream-content-test http-server-test speaker-state-test yeney position-test encoder-test resume-state-test streamer-test upnp-test own-control-test
	./stream-metrics-lifetime-test
	./stream-metrics-lifetime-test worker
	YENEY_STREAM_PACING=sonos YENEY_START_LEAD_MS=1000 ./stream-pacing-encoder-test
	./encoded-backpressure-test
	./stream-pacing-test
	python3 tests/stream_pacing_setting_test.py
	./stream-metrics-test
	python3 tests/stream_pacing_suite_test.py
	python3 tests/remote_selection_test.py
	python3 tests/queue_device_suite_test.py
	python3 tests/queue_device_cli_test.py
	python3 tests/queue_protocol_setting_test.py
	YENEY_ACCEPTANCE_BINARY=$(CURDIR)/yeney python3 tests/queue_binary_test.py
	python3 tests/queue_group_test.py
	python3 tests/queue_title_test.py
	python3 tests/length_binary_test.py
	./queue-source-test
	./room-status-test
	./queue-position-test
	./playback-mode-test
	python3 tests/playback_mode_test.py
	./queue-ownership-test
	./queue-actions-test
	./queue-metadata-test
	python3 tests/artwork_test.py
	python3 tests/xml_metadata_test.py
	./queue-route-test
	./queue-item-store-test
	./core-queue-sink-test
	YENEY_TIMING_PROBE=1 YENEY_TIMING_PUBLISH=1 ./timing-tap-test
	./timing-contract-test
	./cold-position-test
	./field-cold-test
	./oct11-acquisition-test
	./oct12-boundary-test
	YENEY_TIMING_PRIOR=0 ./field-cold-test prior-setting
	YENEY_TIMING_PRIOR=1 ./field-cold-test prior-setting
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
	./xml-sanitize-test
	./stream-content-test
	python3 tests/title_format_setting_test.py
	python3 tests/stream_content_setting_test.py
	python3 tests/list_rooms_test.py
	python3 tests/lms_icon_test.py
	python3 tests/room_state_lookup_test.py
	python3 tests/installer_state_test.py
	python3 tests/debug_settings_test.py
	python3 tests/script_references_test.py
	python3 tests/installer_test.py
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
	python3 tests/pause_mode_test.py
	python3 tests/audio_mode_test.py
	python3 tests/obsolete_player_test.py
	python3 tests/core_sonos_test.py
	python3 tests/player_engine_test.py
	python3 tests/queue_dryrun_test.py
	python3 tests/queue_suite_test.py
	$(MAKE) -C third_party/yeney-core decimator-fixture
	python3 tests/real_lms_test.py
	python3 third_party/yeney-core/tests/decimator_quality.py --sox "$${YENEY_REAL_LMS_HOME:-/tmp/yeney-real-lms-9.1.1}/Bin/x86_64-linux/sox"
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

field-cold-test: tests/field_cold_test.cpp timing_probe_runtime.h timing_probe.h timing_brackets.h timing_settings.h position_state.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $<
test: field-cold-test

oct11-acquisition-test: tests/oct11_acquisition_test.cpp timing_probe_runtime.h timing_probe.h timing_brackets.h timing_drift_state.h timing_settings.h timing_channel.h position_state.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<
test: oct11-acquisition-test

oct12-boundary-test: tests/oct12_boundary_test.cpp core_sonos.cpp core_sonos.h position_state.h timing_probe_runtime.h core_shm.o $(CORE_LIB)
	g++ -std=c++17 -O2 -Wall -Wextra -I. -Ithird_party/yeney-core -pthread -o $@ tests/oct12_boundary_test.cpp core_sonos.cpp core_shm.o $(CORE_LIB) -lFLAC
test: oct12-boundary-test

# Independent phase 1 library; no playback registration or transport changes.
queue_route.o queue_item_store.o wav_writer.o: %.o: %.cpp queue_item_store.h wav_writer.h pcm_pack.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -c -o $@ $<
queue_route.o: queue_route.h
libqueueitems.a: queue_item_store.o wav_writer.o queue_route.o
	ar rcs $@ $^
queue-item-store-test: tests/queue_item_store_test.cpp libqueueitems.a pcm_pack.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $< libqueueitems.a -lFLAC -pthread
test: queue-item-store-test

core_queue_sink.o: core_queue_sink.cpp core_queue_sink.h queue_item_store.h third_party/yeney-core/core/sink.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -c -o $@ $<
core-queue-sink-test: tests/core_queue_sink_test.cpp core_queue_sink.o libqueueitems.a
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread
test: core-queue-sink-test

yeney-queue-dryrun: queue_dryrun.cpp core_queue_sink.o libqueueitems.a $(CORE_LIB)
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread

core-queue-sink-test yeney-queue-dryrun: queue_report.h
yeney-queue-dryrun: queue_lms_cli.h queue_flac_metadata.h

yeney-queue-dryrun: queue_lms_cli.h

test: yeney-queue-dryrun

queue-route-test: tests/queue_route_test.cpp libqueueitems.a upnp/http_server.o upnp/gena.o upnp/xml.o upnp/discovery.o upnp/http.o
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $^ -lFLAC -pthread
test: queue-route-test

queue-metadata-test: tests/queue_metadata_test.cpp upnp/queue_metadata.cpp upnp/xml.cpp upnp/queue_metadata.h upnp/soap.h upnp/title_format.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^)
test: queue-metadata-test

queue-actions-test: tests/queue_actions_test.cpp upnp/queue_actions.cpp upnp/queue_actions.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^)
test: queue-actions-test

queue-ownership-test: tests/queue_ownership_test.cpp upnp/queue_ownership.cpp upnp/queue_ownership.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^)
test: queue-ownership-test

playback-mode-test: tests/playback_mode_test.cpp playback_mode.h upnp/speaker_control.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $<
test: playback-mode-test

QUEUE_CONTROLLER_OBJS = upnp/queue_coordinator.o upnp/queue_actions.o upnp/queue_ownership.o upnp/queue_metadata.o
upnp/queue_coordinator.o: upnp/queue_coordinator.cpp upnp/queue_coordinator.h playback_mode.h upnp/queue_actions.h upnp/queue_ownership.h upnp/http.h upnp/soap.h upnp/xml.h upnp/discovery.h
upnp/queue_actions.o: upnp/queue_actions.cpp upnp/queue_actions.h upnp/soap.h upnp/xml.h
upnp/queue_ownership.o: upnp/queue_ownership.cpp upnp/queue_ownership.h
upnp/queue_metadata.o: upnp/queue_metadata.cpp upnp/queue_metadata.h upnp/soap.h upnp/title_format.h upnp/xml.h
$(QUEUE_CONTROLLER_OBJS):
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -c -o $@ $<
libqueuecontroller.a: $(QUEUE_CONTROLLER_OBJS)
	ar rcs $@ $^
all: libqueuecontroller.a

QUEUE_TEST_UPNP = upnp/http_server.o upnp/gena.o upnp/xml.o upnp/http.o upnp/soap.o upnp/discovery.o



queue-position-test: tests/queue_position_test.cpp position_state.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ $<
test: queue-position-test

upnp/encoded_buffer.o upnp/http_server.o sbstreamer.o sonos-status.o yeney.o: stream_metrics.h

stream-metrics-test: tests/stream_metrics_test.cpp stream_metrics.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<
test: stream-metrics-test

sbencoder.o sbstreamer.o yeney.o sonos-position.o upnp/own_speaker_control.o core_sonos.o: stream_pacing.h

stream-pacing-test: tests/stream_pacing_test.cpp stream_pacing.h timing_probe_runtime.h upnp/encoded_buffer.cpp stream_metrics.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $< upnp/encoded_buffer.cpp
test: stream-pacing-test

encoded-backpressure-test: tests/encoded_backpressure_test.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h stream_metrics.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ tests/encoded_backpressure_test.cpp upnp/encoded_buffer.cpp
test: encoded-backpressure-test

stream-pacing-encoder-test: tests/stream_pacing_encoder_test.cpp sbencoder.cpp sbencoder.h stream_pacing.h stream_metrics.h upnp/encoded_buffer.cpp
	g++ -std=c++17 -O2 -Wall -Wextra -DSBENCODER_TEST -I. -pthread -o $@ tests/stream_pacing_encoder_test.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC
test: stream-pacing-encoder-test

stream-metrics-lifetime-test: tests/stream_metrics_lifetime_test.cpp stream_metrics.h
	g++ -std=c++17 -g -O1 -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer -I. -pthread -o $@ $<
test: stream-metrics-lifetime-test

$(QUEUE_ROOM_OBJS): %.o: %.cpp queue_room_internal.h queue_room.h room_status.h queue_playlist.h queue_lms_cli.h upnp/queue_actions.h core_queue_sink.h
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. -c -o $@ $<



$(OBJS): room_status.h
core_shm.o: third_party/yeney-core/core/sink.h
stream-metrics-test stream-pacing-test encoded-backpressure-test stream-pacing-encoder-test stream-metrics-lifetime-test: room_status.h



yeney.o queue_room.o : lms_artwork.h

queue_room.o: queue_protocol.h

xml-sanitize-test: tests/xml_sanitize_test.cpp upnp/xml.cpp upnp/xml.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -o $@ tests/xml_sanitize_test.cpp upnp/xml.cpp
test: xml-sanitize-test

room-status-test: tests/room_status_test.cpp room_status.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<
test: room-status-test

queue-source-test: tests/queue_source_test.cpp queue_source.h lms_artwork.h
	$(CXX) -std=c++17 -Wall -Wextra -I. -o $@ tests/queue_source_test.cpp
test: queue-source-test

queue_room.o: queue_path_sink.h queue_source.h core_sonos.h

core_sonos.o queue_room.o: lms_icon.h queue_lms_cli.h

lms-icon-test: tests/lms_icon_fixture.cpp lms_icon.h queue_lms_cli.h
	g++ -std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<

lms-icon-connection-test: tests/lms_icon_connection.cpp $(CORE_LIB)
	g++ -std=c++17 -O2 -Wall -Wextra -Ithird_party/yeney-core -pthread -o $@ $< $(CORE_LIB) -lFLAC

test: lms-icon-test lms-icon-connection-test

# Both playback paths and timing persistence share the room state policy.
yeney.o queue_room.o sonos-position.o core_sonos.o sonos-status.o sbencoder.o: room_state_directory.h stream_metrics.h timing_probe_runtime.h timing_drift_state.h

yeney.o upnp/own_speaker_control.o sbstreamer.o: debug_settings.h

$(QUEUE_ROOM_OBJS): queue_room_internal.h queue_room.h core_queue_sink.h queue_path_sink.h queue_playlist.h queue_source.h queue_speaker_changes.h stream_metrics.h

yeney.o sonos-status.o: stream_display.h
yeney.o $(QUEUE_ROOM_OBJS): queue_lms_commands.h

queue_room.o: third_party/yeney-core/core/player.h
