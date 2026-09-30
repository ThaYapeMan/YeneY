# Third-party notices

The single YeneY executable still links squeezelite in both player modes.
Its GPL-3.0 licence and the combined-work licensing paragraph in README.md remain
applicable; selecting core does not remove squeezelite from the executable.

Core mode adds the following components, with their complete attribution and
licence text in the [pinned core notices](third_party/yeney-core/THIRD_PARTY_NOTICES.md):

- yeney-core, PolyForm Noncommercial 1.0.0, pinned by the submodule gitlink.
- minimp3, CC0 1.0 Universal, vendored by yeney-core.
- Apple ALAC decoder, Apache-2.0, recursive submodule of yeney-core; its Apple
  copyright and distribution notice are retained there.
- libFLAC, BSD-3-Clause, linked as the system library (not the GPL CLI sources).

See also [squeezelite's licence](squeezelite/LICENSE.txt) and the retained
[core ALAC distribution notice](third_party/yeney-core/third_party/ALAC_NOTICE).

For this A/B round, YeneY's core host supplies a native MP3 compatibility decoder
using system libmad (GPL-2.0-or-later), with the same precision and LAME handling
as the installed squeezelite mode. This is additional GPL-covered linked code;
the standalone core still defaults to minimp3. On Debian the full libmad licence
and copyright notices are installed at `/usr/share/doc/libmad0/copyright`.
