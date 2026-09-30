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

Core mode uses only yeney-core's decoders: libFLAC (BSD-3-Clause), minimp3
(CC0) and Apple ALAC (Apache-2.0), plus its PCM reader. Squeezelite continues
to load its own codec libraries dynamically; its GPL licensing remains applicable.
