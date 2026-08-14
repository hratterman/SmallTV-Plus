# uzlib (vendored)

Five files from [pfalcon/uzlib](https://github.com/pfalcon/uzlib) (master,
Aug 2026), unmodified: `uzlib.h`, `uzlib_conf.h`, `tinf.h`, `tinf_compat.h`,
`tinflate.c`. zlib-style license, retained in each file's header.

Why it is here: the rain radar inflates RainViewer's PNG tiles, and the
ESP32's ROM copy of miniz — free in flash — costs ~14 KB of RAM for its
decompressor state alone, next to the unavoidable 32 KB dictionary. A field
cube running the miner has ~48 KB of heap for everything; uzlib's state is
about 1.4 KB because it builds its Huffman tables compactly on the fly, and
that difference is what lets the radar decode at all on that cube. Slower
per byte, but a 256 KB tile still inflates in well under a second.
