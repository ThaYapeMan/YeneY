# Third-party notices

Project-owned YeneY code and yeney-core are PolyForm Noncommercial 1.0.0.
The executable incorporates or dynamically links the following components,
which retain their own terms. Complete core decoder attributions and licence
texts are in [the pinned core notices](third_party/yeney-core/THIRD_PARTY_NOTICES.md)
and [Apple's distribution notice](third_party/yeney-core/third_party/ALAC_NOTICE).

| Component | Inclusion | Licence |
| --- | --- | --- |
| yeney-core | Static player library | PolyForm Noncommercial 1.0.0 |
| minimp3 | Core MP3 decoder | CC0 1.0 Universal |
| Apple ALAC | Recursive decoder submodule | Apache-2.0 |
| libFLAC and libFLAC++ | System FLAC decoding/encoding libraries | BSD-3-Clause |
| OpenSSL libcrypto 3 | System random session tokens | Apache-2.0 |
| libogg | Transitive libFLAC dependency | BSD-3-Clause |
| zlib | Transitive system dependency | zlib licence |
| Zstandard libzstd | Transitive system dependency | BSD-3-Clause alternative |
| GNU libc, libm and dynamic loader | System runtime, including pthread | LGPL-2.1-or-later (individual files have additional permissive terms) |
| GNU libstdc++ and libgcc_s | C++/compiler runtime | GPL-3.0 with GCC Runtime Library Exception 3.1 |

The final direct link is `libyeneycore.a -lFLAC++ -lFLAC -lcrypto -lpthread`.
The standard C++ compiler also supplies its runtime libraries. `ldd` on the
validated Linux build records the actual transitive dependencies; another
system's packaging may differ. No obsolete player or its dynamic codec loaders
are linked. The FLAC command-line tool is used only by tests, never incorporated
into the executable.

The GCC runtime exception permits independent non-GPL programs to use its covered
runtime under the stated conditions; these runtime dependencies must not be
misrepresented as having no GPL terms. See the [GCC licence documentation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html).
See also [FLAC library licensing](https://xiph.org/flac/license.html),
[OpenSSL 3 licensing](https://openssl-library.org/source/license/index.html),
and the installed system packages' copyright files for complete system notices.
