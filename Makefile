# Test installations persist on disk; each test invocation owns a cleaned workspace.
export YENEY_TEST_CACHE ?= $(HOME)/.cache/yeney-test
export TMPDIR ?= /var/tmp
# An explicitly empty caller value disables private-media checks.
export YENEY_LENGTH_FIXTURE_DIR ?= $(shell for directory in "$(HOME)/src/private-fixtures" "$(HOME)/private-fixtures" /mnt/c/HueSyncLXC/private-fixtures; do if test -d "$$directory"; then printf "%s" "$$directory"; break; fi; done)
export LAMPASTREAM_CHECKOUT ?= $(abspath ../LampaStream)

OWN_UPNP_SOURCES = upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/http.cpp upnp/soap.cpp upnp/discovery.cpp upnp/own_speaker_control.cpp
UPNP_OBJS = $(OWN_UPNP_SOURCES:.cpp=.o) upnp/encoded_buffer.o

QUEUE_ROOM_OBJS = queue_room.o queue_relay.o queue_path.o queue_speaker.o queue_lms_commands.o queue_status.o queue_window_relay.o queue_native_decoder.o

OBJS = speaker_websocket.o volume_control.o $(QUEUE_ROOM_OBJS) core_queue_sink.o core_sonos.o core_shm.o audio_mode.o $(UPNP_OBJS) yeney.o sbstreamer.o sbencoder.o sonos-status.o sonos-position.o

all: yeney libqueueitems.a yeney-queue-dryrun libqueuecontroller.a

DEPFLAGS = -MMD -MP -MF $@.d

.DELETE_ON_ERROR:
# Compile-and-link commands can overwrite dependency output for each source.
# Ask the compiler to emit dependencies for all translation units in one pass.
define compile-program
$1 $(filter-out -o $@ -l% -Wl% %.o %.a,$2) -MM -MP -MT '$@' > $@.d.tmp
$1 $2
mv $@.d.tmp $@.d
endef

.PHONY: version-refresh
version-refresh: bridge_version.h
ifneq ($(shell python3 scripts/build-version.py --check),current)
bridge_version.h: FORCE
endif
bridge_version.h: | VERSION scripts/build-version.py
	python3 scripts/build-version.py

queue-native-decoder-test: tests/queue_native_decoder_test.cpp queue_native_decoder.cpp
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. tests/queue_native_decoder_test.cpp queue_native_decoder.cpp -pthread -o $@)

CORE_LIB = third_party/yeney-core/libyeneycore.a
.PHONY: core-library
core-library:
$(CORE_LIB): core-library
	$(MAKE) -C third_party/yeney-core libyeneycore.a

core_sonos.o: %.o: %.cpp
	g++ -std=c++17 -g -O3 -Wall -Wextra -Ithird_party/yeney-core $(DEPFLAGS) -c -o $@ $<

yeney: $(OBJS) libqueuecontroller.a libqueueitems.a $(CORE_LIB)
	g++ -g -o $@ $^ \
		-lFLAC++ -lFLAC -lssl -lcrypto \
		-lpthread

clean:
	rm -f $(BRIDGE_OBJS) $(PROGRAMS) yeney libqueueitems.a libqueuecontroller.a \
		$(DEPS) $(addsuffix .tmp,$(DEPS)) bridge_version.h bridge_version.h.tmp .build-flags .build-flags.tmp
	$(MAKE) -C third_party/yeney-core clean

.PHONY: test test-run install
test:
	python3 scripts/run-tests.py python3 scripts/test-runner.py --mode full

install: yeney-queue-dryrun smoke
	install -d -m 0755 /var/lib/yeney
	install -m 0755 yeney-queue-dryrun /usr/local/bin/yeney-queue-dryrun
	install -m 0755 scripts/yeney-queue-suite /usr/local/bin/yeney-queue-suite
	install -d -m 0755 /usr/local/lib/yeney
	install -m 0644 scripts/install_devices.py /usr/local/lib/yeney/install_devices.py
	install -m 0755 scripts/yeney-format /usr/local/bin/yeney-format
	install -m 0644 scripts/yeney_room_state.py /usr/local/bin/yeney_room_state.py
	install -m 0755 scripts/yeney-stream-pacing-suite /usr/local/bin/yeney-stream-pacing-suite
	install -m 0755 scripts/yeney-queue-device-suite /usr/local/bin/yeney-queue-device-suite
	install -d -m 0755 /usr/local/share/yeney
	install -m 0644 assets/sonos.svg /usr/local/share/yeney/sonos.svg
	install -m 0755 scripts/yeney-install-lms-icon /usr/local/bin/yeney-install-lms-icon
	scripts/install-devices.sh $(if $(LMS_PREFS_DIR),--lms-prefs-dir="$(LMS_PREFS_DIR)")
encoder-test: tests/audio_pack_fixture.cpp upnp/encoded_buffer.cpp tests/encoder_test.cpp sbencoder.cpp
	$(call compile-program,g++,-g -O2 -Wall -I. -DSBENCODER_TEST -o $@ tests/encoder_test.cpp tests/audio_pack_fixture.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread)

resume-state-test: tests/resume_state_test.cpp
	$(call compile-program,g++,-g -O2 -Wall -I. -o $@ tests/resume_state_test.cpp)

streamer-test: upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/encoded_buffer.cpp tests/streamer_test.cpp sbstreamer.cpp sbencoder.cpp
	$(call compile-program,g++,-g -O2 -Wall -I. -o $@ tests/streamer_test.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread)

position-test: tests/position_test.cpp
	$(call compile-program,g++,-g -O2 -Wall -I. -o $@ tests/position_test.cpp)
streamer-test: sonos-position.cpp

upnp-test: tests/upnp_test.cpp upnp/xml.cpp upnp/soap.cpp upnp/http.cpp upnp/discovery.cpp
	$(call compile-program,g++,-g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^))

own-control-test: tests/own_control_fixture.cpp $(OWN_UPNP_SOURCES)
	$(call compile-program,g++,-g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread)

speaker-state-test: tests/speaker_state_test.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp
	$(call compile-program,g++,-g -O2 -Wall -Wextra -I. -o $@ tests/speaker_state_test.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp)

http-server-test: tests/http_server_fixture.cpp upnp/http_server.cpp
	$(call compile-program,g++,-g -O2 -Wall -Wextra -I. -o $@ tests/http_server_fixture.cpp upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp -lpthread)

http-server-test: upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp

stream-content-test: tests/stream_content_test.cpp upnp/soap.cpp upnp/xml.cpp
	$(call compile-program,g++,-g -O2 -Wall -Wextra -I. -o $@ tests/stream_content_test.cpp upnp/soap.cpp upnp/xml.cpp)

encoded-buffer-test: tests/encoded_buffer_test.cpp upnp/encoded_buffer.cpp
	$(call compile-program,g++,-g -O2 -Wall -Wextra -I. -o $@ tests/encoded_buffer_test.cpp upnp/encoded_buffer.cpp -lpthread)

range-history-test: tests/range_history_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -I. -o $@ $<)
range-test: tests/range_fixture.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp upnp/encoded_buffer.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -I. -o $@ $(filter %.cpp,$^) -lFLAC++ -lFLAC -lcrypto -lpthread)

lead-test: tests/lead_fixture.cpp sbencoder.cpp upnp/encoded_buffer.cpp
	$(call compile-program,g++,-g -O2 -Wall -I. -o $@ tests/lead_fixture.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread)

# Measurement-only timing probe fixtures.
timing-probe-test: tests/timing_probe_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $<)
timing-probe-resets-test: tests/timing_probe_resets_test.cpp sonos-position.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread)
timing-probe-http-test: tests/timing_probe_http_fixture.cpp $(OWN_UPNP_SOURCES)
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread)

timing-probe-field-test: tests/timing_probe_field_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $<)
timing-probe-settings-test: tests/timing_probe_settings_fixture.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $< -lpthread)

timing-contract-test: tests/timing_contract_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

timing-tap-test: tests/timing_tap_test.cpp core_shm.o $(CORE_LIB)
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -Ithird_party/yeney-core -pthread -o $@ $< core_shm.o $(CORE_LIB) -lFLAC)

core_shm.o: third_party/yeney-core/sinks/shm_v1/sink.cpp
	g++ -std=c++17 -O2 -Wall -Wextra -Ithird_party/yeney-core $(DEPFLAGS) -c -o $@ $<

lms-position-test: tests/lms_position_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

cold-position-test: tests/cold_position_test.cpp
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -I. -o $@ $<)

field-cold-test: tests/field_cold_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $<)

oct11-acquisition-test: tests/oct11_acquisition_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

oct12-boundary-test: tests/oct12_boundary_test.cpp core_sonos.cpp core_shm.o $(CORE_LIB)
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -Ithird_party/yeney-core -pthread -o $@ tests/oct12_boundary_test.cpp core_sonos.cpp core_shm.o $(CORE_LIB) -lFLAC)

queue_route.o queue_item_store.o wav_writer.o flac_writer.o packed_flac.o: %.o: %.cpp
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. $(DEPFLAGS) -c -o $@ $<
QUEUE_ITEM_OBJS = queue_item_store.o wav_writer.o flac_writer.o packed_flac.o queue_route.o
# Recreate the archive so incremental builds use the same member order as clean builds.
libqueueitems.a: $(QUEUE_ITEM_OBJS)
	rm -f $@.tmp
	ar rcs $@.tmp $^
	mv $@.tmp $@
queue-row-order-test: tests/queue_row_order_test.cpp libqueueitems.a
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. $< libqueueitems.a -lFLAC -pthread -o $@)

queue-item-store-test: tests/queue_item_store_test.cpp libqueueitems.a
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $< libqueueitems.a -lFLAC -pthread)

core_queue_sink.o: core_queue_sink.cpp
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. $(DEPFLAGS) -c -o $@ $<
queue-declaration-test: tests/queue_declaration_test.cpp core_queue_sink.o libqueueitems.a $(CORE_LIB) upnp/queue_metadata.cpp upnp/xml.o
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -Itests -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread)
core-queue-sink-test: tests/core_queue_sink_test.cpp core_queue_sink.o libqueueitems.a
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread)

yeney-queue-dryrun: queue_dryrun.cpp core_queue_sink.o libqueueitems.a $(CORE_LIB)
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread)

queue-flac-writer-test: tests/queue_flac_writer_test.cpp flac_writer.o packed_flac.o
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o,$^) -lFLAC -lcrypto)

queue-flac-http-test: tests/queue_flac_http_test.cpp libqueueitems.a upnp/http_server.o upnp/gena.o upnp/xml.o upnp/discovery.o upnp/http.o
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread)

queue-route-test: tests/queue_route_test.cpp libqueueitems.a upnp/http_server.o upnp/gena.o upnp/xml.o upnp/discovery.o upnp/http.o
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread)

queue-metadata-test: tests/queue_metadata_test.cpp upnp/queue_metadata.cpp upnp/xml.cpp
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^))

queue-actions-test: tests/queue_actions_test.cpp upnp/queue_actions.cpp
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^))

queue-ownership-test: tests/queue_ownership_test.cpp upnp/queue_ownership.cpp
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^))

playback-mode-test: tests/playback_mode_test.cpp
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $<)

QUEUE_CONTROLLER_OBJS = upnp/queue_coordinator.o upnp/queue_actions.o upnp/queue_ownership.o upnp/queue_metadata.o
upnp/queue_coordinator.o: upnp/queue_coordinator.cpp
upnp/queue_actions.o: upnp/queue_actions.cpp
upnp/queue_ownership.o: upnp/queue_ownership.cpp
upnp/queue_metadata.o: upnp/queue_metadata.cpp
$(QUEUE_CONTROLLER_OBJS):
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. $(DEPFLAGS) -c -o $@ $<
libqueuecontroller.a: $(QUEUE_CONTROLLER_OBJS)
	ar rcs $@ $^
queue-position-test: tests/queue_position_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $<)

stream-metrics-test: tests/stream_metrics_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

stream-pacing-test: tests/stream_pacing_test.cpp upnp/encoded_buffer.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $< upnp/encoded_buffer.cpp)

encoded-backpressure-test: tests/encoded_backpressure_test.cpp upnp/encoded_buffer.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ tests/encoded_backpressure_test.cpp upnp/encoded_buffer.cpp)

stream-pacing-encoder-test: tests/stream_pacing_encoder_test.cpp sbencoder.cpp upnp/encoded_buffer.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -DSBENCODER_TEST -I. -pthread -o $@ tests/stream_pacing_encoder_test.cpp sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC)

stream-metrics-lifetime-test: tests/stream_metrics_lifetime_test.cpp
	$(call compile-program,g++,-std=c++17 -g -O1 -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer -I. -pthread -o $@ $<)

$(QUEUE_ROOM_OBJS): %.o: %.cpp
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. $(DEPFLAGS) -c -o $@ $<

xml-sanitize-test: tests/xml_sanitize_test.cpp upnp/xml.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ tests/xml_sanitize_test.cpp upnp/xml.cpp)

room-status-test: tests/room_status_test.cpp
	$(call compile-program,$(CXX),-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

queue-source-test: tests/queue_source_test.cpp
	$(call compile-program,$(CXX),-std=c++17 -Wall -Wextra -I. -o $@ tests/queue_source_test.cpp)

lms-icon-test: tests/lms_icon_fixture.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

lms-icon-connection-test: tests/lms_icon_connection.cpp $(CORE_LIB)
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -Ithird_party/yeney-core -pthread -o $@ $< $(CORE_LIB) -lFLAC)

volume-test: tests/volume_test.cpp volume_control.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ tests/volume_test.cpp volume_control.cpp)

podcast-sink-test: tests/podcast_sink_test.cpp core_queue_sink.o libqueueitems.a
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -lFLAC -pthread)

podcast-metadata-fixture: tests/podcast_metadata_fixture.cpp libqueuecontroller.a upnp/xml.o
	$(call compile-program,$(CXX),-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $< libqueuecontroller.a upnp/xml.o)

queue-window-test: tests/queue_window_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $<)

consumption-diagnostic-test: tests/consumption_diagnostic_test.cpp
	$(call compile-program,$(CXX),-std=c++17 -O2 -Wall -Wextra -I. -pthread -o $@ $<)

.PHONY: navigation-soak navigation-targeted

seek-worker-test: tests/seek_worker_test.cpp core_queue_sink.o libqueueitems.a
	$(call compile-program,g++,-std=c++17 -g -O2 -Wall -Wextra -fno-access-control -I. -Itests -o $@ tests/seek_worker_test.cpp core_queue_sink.o libqueueitems.a -lFLAC -pthread)

.PHONY: full-queue-pressure full-queue-pressure-soak

.PHONY: full-queue-acceptance
full-queue-acceptance: yeney
	YENEY_FULLQ=1 YENEY_FULLQ_PAUSED=1 python3 tests/real_lms_test.py
	@set -e; for codec in flac alac wav; do for rows in 12 100 500; do \
	    YENEY_FULLQ=1 YENEY_FULLQ_CODEC=$$codec YENEY_FULLQ_ROWS=$$rows python3 tests/real_lms_test.py; \
	done; done
	YENEY_FULLQ=1 YENEY_FULLQ_GENERATION=1 python3 tests/real_lms_test.py
	YENEY_FULLQ=1 YENEY_FULLQ_QUOTA_MIB=24 python3 tests/real_lms_test.py
	YENEY_FULLQ=1 YENEY_FULLQ_REJECT_BATCHES=1 python3 tests/real_lms_test.py
	YENEY_REAL_LMS_NAV_INVARIANTS=1 YENEY_NAV_CASE=B1 YENEY_FULLQ_Q8=1 python3 tests/real_lms_test.py

.PHONY: test-fast test-fast-run
test-fast:
	$(MAKE) -C third_party/yeney-core format-check
	python3 scripts/run-tests.py python3 scripts/test-runner.py --mode fast --serial

speaker_websocket.o: speaker_websocket.cpp
	g++ -std=c++17 -g -O2 -Wall -Wextra -I. $(DEPFLAGS) -c -o $@ $<

speaker-position-fit-test: tests/speaker_position_fit_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. $< -o $@)

speaker-position-replay-test: tests/speaker_position_replay_test.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. $< -o $@)

speaker-websocket-fixture: tests/speaker_websocket_fixture.cpp speaker_websocket.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. tests/speaker_websocket_fixture.cpp speaker_websocket.cpp -lssl -lcrypto -pthread -o $@)

websocket-timer-fixture: tests/websocket_timer_fixture.cpp speaker_websocket.cpp
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. tests/websocket_timer_fixture.cpp -lssl -lcrypto -pthread -o $@)

.PHONY: speaker-position-test

native-mp4-fixture: tests/native_mp4_fixture.cpp queue_native_decoder.cpp $(CORE_LIB)
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. tests/native_mp4_fixture.cpp queue_native_decoder.cpp $(CORE_LIB) -lFLAC -pthread -o $@)

.PHONY: native-mp4-test

native-pcm-fixture: tests/native_pcm_fixture.cpp queue_native_decoder.cpp $(CORE_LIB)
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. tests/native_pcm_fixture.cpp queue_native_decoder.cpp $(CORE_LIB) -lFLAC -pthread -o $@)

.PHONY: native-pcm-test

.PHONY: test-serial test-run test-fast-run test-build test-fast-build
test-serial:
	python3 scripts/run-tests.py python3 scripts/test-runner.py --mode full --serial
test-run:
	python3 scripts/test-runner.py --mode full
test-fast-run:
	python3 scripts/test-runner.py --mode fast --serial
test-build:
	python3 scripts/test-runner.py --mode full --build-only
test-fast-build:
	python3 scripts/test-runner.py --mode fast --build-only

.PHONY: full-queue-pressure
full-queue-pressure:
	python3 scripts/test-runner.py --selection full-queue-pressure

.PHONY: full-queue-pressure-soak
full-queue-pressure-soak:
	python3 scripts/test-runner.py --selection full-queue-pressure-soak

.PHONY: native-mp4-test
native-mp4-test:
	python3 scripts/test-runner.py --selection native-mp4-test

.PHONY: native-pcm-test
native-pcm-test:
	python3 scripts/test-runner.py --selection native-pcm-test

.PHONY: navigation-soak
navigation-soak:
	python3 scripts/test-runner.py --selection navigation-soak

.PHONY: navigation-targeted
navigation-targeted:
	python3 scripts/test-runner.py --selection navigation-targeted

.PHONY: speaker-position-test
speaker-position-test:
	python3 scripts/test-runner.py --selection speaker-position-test


BRIDGE_OBJS = $(OBJS) $(QUEUE_CONTROLLER_OBJS) $(QUEUE_ITEM_OBJS)
EXPLICIT_OBJS = speaker_websocket.o core_sonos.o core_shm.o core_queue_sink.o $(QUEUE_ROOM_OBJS) $(QUEUE_CONTROLLER_OBJS) $(QUEUE_ITEM_OBJS)
TEST_PROGRAMS = queue-native-decoder-test encoder-test resume-state-test streamer-test position- \
    test upnp-test own-control-test speaker-state-test http-server-test stream-content-test \
    encoded-buffer-test range-history-test range-test lead-test timing-probe-test timing-probe- \
    resets-test timing-probe-http-test timing-probe-field-test timing-probe-settings-test timing- \
    contract-test timing-tap-test lms-position-test cold-position-test field-cold-test \
    oct11-acquisition-test oct12-boundary-test queue-row-order-test queue-item-store-test queue- \
    declaration-test core-queue-sink-test queue-route-test queue-metadata-test queue-actions-test \
    queue-ownership-test playback-mode-test queue-position-test stream-metrics-test stream-pacing- \
    test encoded-backpressure-test stream-pacing-encoder-test stream-metrics-lifetime-test xml- \
    sanitize-test room-status-test queue-source-test lms-icon-test lms-icon-connection-test volume- \
    test podcast-sink-test podcast-metadata-fixture queue-window-test consumption-diagnostic-test \
    seek-worker-test speaker-position-fit-test speaker-position-replay-test speaker-websocket- \
    fixture websocket-timer-fixture native-mp4-fixture native-pcm-fixture queue-flac-writer-test queue-flac-http-test
PROGRAMS = yeney-queue-dryrun yeney-smoke-server $(TEST_PROGRAMS)
DEP_TARGETS = $(BRIDGE_OBJS) $(PROGRAMS)
DEPS = $(addsuffix .d,$(DEP_TARGETS))

$(filter-out $(EXPLICIT_OBJS),$(BRIDGE_OBJS)): %.o: %.cpp
	$(COMPILE.cpp) $(DEPFLAGS) $(OUTPUT_OPTION) $<

.PHONY: FORCE smoke
FORCE:

# Preserve the stamp's timestamp when the effective flags and core pin agree.
quote = '$(subst ','"'"',$1)'
core_pin := $(shell git rev-parse HEAD:third_party/yeney-core)
build_flags := $(call quote,$(CXX)) $(call quote,$(CXXFLAGS)) $(call quote,$(CPPFLAGS)) \
    $(call quote,$(TARGET_ARCH)) $(call quote,$(COMPILE.cpp)) $(call quote,$(OUTPUT_OPTION)) $(core_pin)
ifneq ($(shell printf '%s\n' $(build_flags) | cmp -s - .build-flags && printf current),current)
.build-flags: FORCE
endif
.build-flags:
	@printf '%s\n' $(build_flags) > $@.tmp
	@mv -f $@.tmp $@

$(DEP_TARGETS): Makefile .build-flags | bridge_version.h
$(patsubst %.d,%,$(filter-out $(wildcard $(DEPS)),$(DEPS))): FORCE

yeney-smoke-server: tests/queue_smoke_server.cpp libqueueitems.a upnp/http_server.o upnp/gena.o upnp/xml.o upnp/discovery.o upnp/http.o
	$(call compile-program,g++,-std=c++17 -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp %.o %.a,$^) -Wl$(comma)--wrap=bind -lFLAC -pthread)

comma := ,
smoke: yeney yeney-smoke-server
	@./yeney-smoke-server

-include $(DEPS)
