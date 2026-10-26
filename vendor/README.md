# Third-party code

| Component | Source | License |
| --- | --- | --- |
| Fathom by Ronald de Man, Basil Falcinelli, Jon Dart and contributors | [c9c6fef](https://github.com/jdart1/Fathom/tree/c9c6fef0dddc05d2e242c183acf5833149ab676d) | [MIT](fathom/LICENSE) |
| Gaviota by Miguel A. Ballicora | [981472c](https://github.com/michiguel/Gaviota-Tablebases/tree/981472cc83e3a8b6e996191e564295609ea4ce30) | [MIT/X11 and compression notices](gaviota/license.txt) |
| LibYAML by Kirill Simonov and contributors | [0.2.5](libyaml/UPSTREAM.md) | [MIT](libyaml/License) |
| python-chess by Niklas Fiekas and contributors | [0c6bdac](https://github.com/niklasf/python-chess/tree/0c6bdaccfcb3b09cdbe94b0f81e948cb2a356015) | [GPLv3 or later](python-chess-LICENSE) |

Gaviota includes LZMA by Igor Pavlov (public domain), zlib by Jean-loup Gailly
and Mark Adler (zlib license), LZF by Marc Alexander Lehmann
([BSD license](gaviota/compression/liblzf/LICENSE)), and its Huffman implementation.

Variant Syzygy code in `src/` is adapted from python-chess.
Syzygy format and indexing: Ronald de Man. Polyglot constants: Fabien Letouzey.

[tablebases.mk](tablebases.mk) renames Gaviota's `tb_init` to `gtb_init` and hides
compression symbols to avoid Fathom/libcurl conflicts.
[Test table sources](../tests/fixtures/SOURCES.md).
