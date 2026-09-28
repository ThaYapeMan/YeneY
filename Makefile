FLAGS_SL = -g -O3 -Wall -fno-common -Isqueezelite -Wno-error=incompatible-pointer-types -fpermissive

OWN_UPNP_SOURCES = upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/http.cpp upnp/soap.cpp upnp/discovery.cpp upnp/own_speaker_control.cpp
UPNP_OBJS = $(OWN_UPNP_SOURCES:.cpp=.o) upnp/encoded_buffer.o

OBJS = audio_mode.o $(UPNP_OBJS) sonos-lms.o sbstreamer.o sbencoder.o sonos-status.o sonos-position.o

OBJS_SL = squeezelite.o \
	output_sonos.o \
	slimproto_sonos.o \
	squeezelite/decode.o \
	squeezelite/buffer.o \
	squeezelite/stream.o \
	squeezelite/utils.o \
	squeezelite/output.o \
	squeezelite/output_pack.o \
	squeezelite/flac.o \
	squeezelite/pcm.o \
	squeezelite/vorbis.o \
	squeezelite/faad.o \
	squeezelite/mad.o \
	squeezelite/mpg.o

all: sonos-lms

%.o: %.cpp
	g++ -g -O3 -Wall -c -o $@ $<

%.o: %.c
	gcc $(FLAGS_SL) -c -o $@ $<

squeezelite.o: squeezelite.cpp
	g++ $(FLAGS_SL) -c -o $@ $<

sonos-lms: $(OBJS) $(OBJS_SL)
	g++ -g -o $@ $^ \
		-lFLAC++ -lFLAC -lcrypto \
		-lpthread -lm -lrt -ldl -lasound

clean:
	rm -f encoded-buffer-test stream-content-test http-server-test speaker-state-test *.o tests/*.o upnp/*.o squeezelite/*.o sonos-lms position-test encoder-test resume-state-test streamer-test upnp-test own-control-test

slimproto_sonos.o: slimproto_sonos.c squeezelite/slimproto.c squeezelite/squeezelite.h

.PHONY: test install
install: sonos-lms
	scripts/install-devices.sh
encoder-test: tests/audio_pack_fixture.o squeezelite/output_pack.o upnp/encoded_buffer.cpp upnp/encoded_buffer.h tests/encoder_test.cpp sbencoder.cpp sbencoder.h
	g++ -g -O2 -Wall -I. -DSBENCODER_TEST -o $@ tests/encoder_test.cpp tests/audio_pack_fixture.o squeezelite/output_pack.o sbencoder.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread

test: encoded-buffer-test stream-content-test http-server-test speaker-state-test sonos-lms position-test encoder-test resume-state-test streamer-test upnp-test own-control-test
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
	python3 tests/upnp_mock_test.py
	python3 tests/own_display_test.py
	./position-test
	python3 tests/position_reconnect_test.py
	python3 tests/send_error_test.py
	./streamer-test session
	./streamer-test position
	./streamer-test shutdown
	python3 tests/output_shutdown_test.py
	./encoder-test
	./resume-state-test
	SONOS_LMS_PAUSE=pause ./streamer-test
	env -u SONOS_LMS_PAUSE ./streamer-test stop
	python3 tests/http_streamer_test.py
	python3 tests/device_resume_test.py
	python3 tests/yeney_timeline_test.py
	python3 tests/lms_discovery_test.py
	python3 tests/device_test_script_test.py
	python3 tests/pause_mode_test.py
	python3 tests/audio_mode_test.py
	python3 tests/audio_output_test.py

sbstreamer.o sbencoder.o: sbencoder.h

sonos-lms.o: resume_state.h stop_debounce.h
resume-state-test: tests/resume_state_test.cpp resume_state.h stop_debounce.h
	g++ -g -O2 -Wall -I. -o $@ tests/resume_state_test.cpp

streamer-test: upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h pause_mode.h tests/streamer_test.cpp sbstreamer.cpp sbstreamer.h sbencoder.cpp sbencoder.h resume_state.h
	g++ -g -O2 -Wall -I. -o $@ tests/streamer_test.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/encoded_buffer.cpp -lFLAC++ -lFLAC -lcrypto -lpthread

sonos-lms.o streamer-test: pause_mode.h

position-test: tests/position_test.cpp position_state.h
	g++ -g -O2 -Wall -I. -o $@ tests/position_test.cpp
sonos-position.o: position_state.h sonos-position.h
output_sonos.o sonos-lms.o sbstreamer.o streamer-test: sonos-position.h
streamer-test: sonos-position.cpp position_state.h

sonos-lms.o: transport_intent.h retry_budget.h

sonos-lms.o sbstreamer.o streamer-test: stream_session.h

$(OBJS) streamer-test: upnp/speaker_control.h upnp/stream_server.h

$(UPNP_OBJS) sonos-lms.o: $(wildcard upnp/*.h)

upnp-test: $(wildcard upnp/*.h) tests/upnp_test.cpp upnp/xml.cpp upnp/soap.cpp upnp/http.cpp upnp/discovery.cpp
	g++ -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^)

own-control-test: $(wildcard upnp/*.h) tests/own_control_fixture.cpp $(OWN_UPNP_SOURCES)
	g++ -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread

upnp/encoded_buffer.o sbencoder.o: upnp/encoded_buffer.h

sonos-lms.o: upnp/list_rooms.h

sbstreamer.o streamer-test: stream_close_log.h

sonos-lms.o sonos-status.o: sonos-status.h
sonos-status.o: speaker_uri.h stream_session.h

output_sonos.o slimproto_sonos.o audio_mode.o sonos-lms.o sbstreamer.o streamer-test: audio_mode.h

speaker-state-test: tests/speaker_state_test.cpp $(wildcard upnp/*.h) upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/speaker_state_test.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp

sonos-lms.o sbstreamer.o streamer-test resume-state-test: upnp/timing.h

http-server-test: $(wildcard upnp/*.h) tests/http_server_fixture.cpp upnp/http_server.cpp upnp/http_server.h upnp/stream_server.h upnp/timing.h
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/http_server_fixture.cpp upnp/http_server.cpp upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp -lpthread

http-server-test: upnp/gena.cpp upnp/xml.cpp upnp/discovery.cpp upnp/http.cpp upnp/icon.h

stream-content-test: tests/stream_content_test.cpp upnp/soap.cpp upnp/xml.cpp $(wildcard upnp/*.h)
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/stream_content_test.cpp upnp/soap.cpp upnp/xml.cpp

encoded-buffer-test: tests/encoded_buffer_test.cpp upnp/encoded_buffer.cpp upnp/encoded_buffer.h
	g++ -g -O2 -Wall -Wextra -I. -o $@ tests/encoded_buffer_test.cpp upnp/encoded_buffer.cpp -lpthread

upnp/http_server.o streamer-test own-control-test: upnp/icon.h
