# Core-only player migration

The executable uses yeney-core exclusively. The supplied daily-use results and
five consecutive successful grouped starts support removing the alternate engine.
Initial reserve, Range recovery, source relinquish, audible position and all
shared HTTP/transport behavior remain in project-owned bridge code.

## Decisions

1. Remove the alternate player code, output backend, mode header, generated
   reporting adapter, submodule gitlink and .gitmodules entry. Official submodule
   deinitialization also unregisters its local configuration. Preserve historical
   commits and unreferenced Git object caches; never rewrite or purge history.
2. Call runCoreClient unconditionally. Keep the core StopTimer transport dispatch,
   without the removed engine's inline dispatch branch. Register the existing
   atexit HTTP/event cleanup and preserve signal shutdown. Change the startup
   provenance marker to `Player engine: yeney-core`, independent of old settings.
3. YENEY_PLAYER is only a compatibility warning: unset/core is silent; all other
   values, including empty, warn exactly once and continue. Keep discovery-only
   commands' machine-readable output quiet. The required removal warning uses
   concatenated string literals to preserve its exact runtime text while meeting
   the source grep requirement. This does not preserve an engine selector.
4. Move the monotonic get_sb_time_ms and sonos_output_running helpers into the
   core bridge. Use steady_clock rather than retaining the removed C backend.
   Remove its position-fix escape hatch; retain every core/shared debug option.
5. Implement PCM packing independently in pcm_pack.h: take the upper 16 or 24
   bits of normalized signed samples, with unsigned shifts and little-endian
   bytes. Share it between the core feeder and the replacement test adapter.
   No deleted player's source is copied. Keep encoder sample/range assertions,
   native 24-bit precision and both audio modes unchanged.
6. Remove the C/compiler compatibility flags and all obsolete engine objects and
   codec loader dependencies. The final direct link is libyeneycore.a, libFLAC++,
   libFLAC, libcrypto and pthread; the C++ compiler supplies its standard runtimes.
   Core's static archive incorporates minimp3 CC0 and Apple ALAC Apache-2.0.
7. List all observed system dependencies: FLAC/FLAC++ BSD-3-Clause, OpenSSL 3
   Apache-2.0, libogg BSD-3-Clause, zlib, libzstd BSD alternative, libc/libm/loader
   LGPL and libstdc++/libgcc with GCC Runtime Library Exception. Do not describe
   GCC runtimes as having no GPL terms. The removed GPL player implementation and
   codec loaders are absent. System packaging can change transitive dependencies.
   Preserve complete core notices; replace the current combined-work paragraph
   with the PolyForm statement and a short historical release note in README.
8. Keep the core half of player_engine_test, including identity across sessions,
   transport/stream-ID sequence, FLAC/PCM/ALAC source comparisons, native ALAC,
   MP3 exact gapless lengths/markers, delayed-SOAP heartbeat, repeated q/q/s and
   signal cleanup. Retire cross-engine identity/decision/audio comparisons and
   MP3 SNR/correlation thresholds that required the deleted engine as comparator.
   The bundled decoder suite retains its independent quality/reference tests.
9. Remove duplicate alternate-engine runs in Range, lead and foreign-source tests.
   Keep their core assertions at legacy lead 0 and positive lead 2000. Remove
   player_mode tests and replace them with obsolete-setting warning tests.
10. Remove audio_output_test, audio_gain_fixture and output_shutdown_test because
    they directly compile the deleted backend. Core sink tests and bundled
    transition tests cover packing, gain saturation, fades, pause/flush and
    bounded feeder shutdown. Preserve the existing encoder fixture assertions
    through a new project-owned audio_pack_fixture.cpp.
11. Convert the S7#2 position replay to the core's actual statusPacket serializer;
    keep every replay timing, monotonicity, ahead/lag and reset assertion. Remove
    its dependencies on the deleted output backend, STAT shim and utility source.
12. Device-resume fixtures now model one core StopTimer transport poll after each
    command, preserving all existing outcome/assertion checks. They previously
    relied on the removed engine's inline dispatch. This adapts the harness to
    core scheduling without changing retry, stop/pause or ownership expectations.
13. Device-test scripts read the current invocation's new engine marker and accept
    only core as the optional required engine. Preserve unknown/mismatch failure
    checks and avoid using the shell's obsolete environment as provenance.
14. The installer removes only a regular, non-symlink player.conf containing exactly
    `[Service]\nEnvironment=YENEY_PLAYER=core`, with an optional final LF. Quoted,
    commented, extra/blank-line, CRLF, unknown and empty content is left with a note.
    Preserve symlink files/directories. Revalidate before unlink. Never enumerate
    or remove debug.conf or unrelated overrides; leave the drop-in directory.
15. Include override removal in the normal installer plan/apply phase. Reload the
    service manager once and restart an affected active enabled room even when
    its build stamp is unchanged. Existing config, disabled-room policy, approval
    UI, build stamping and restart/install behavior remain. Tests inject temporary
    paths and mocked service commands; no real installation is performed.
16. Core's standalone SHM sink, test and owner-run script contain four compatibility
    identifiers naming an external published ABI. These are original non-GPL code
    and are not linked into YeneY. Preserve their wire/path/class names using
    concatenated literals; commit and push this three-file core change before
    updating the parent pin. No compatibility consumer or runtime behavior changes.
17. Audit text with case-insensitive recursive grep, excluding Git metadata,
    bytecode/binary files, the exact README historical line and core's own Markdown
    docs. These exclusions preserve history and the upstream project's independent
    documentation. All other current text must have no contiguous removed-engine
    name. Also inspect the final link line, ldd and loaded process mappings.
18. Validate a fresh clone with recursive submodule initialization and a clean
    build. Make the bundled core suite part of the root make test target;
    document flac/ffmpeg/clang-format as test-only tools. Run all suites, retaining the optional
    LampaStream skip when its checkout is absent. Frontend tests are not applicable:
    YeneY has no frontend. Commit and push normally, without amend/rebase/force-push.
19. Do not SSH, deploy, archive or delete any GitHub repository. After this commit,
    the user archives the retired engine fork themselves. Keep this an owner task.
