# The LZMA SDK, as iTwin ships it

A changeset file is an LZMA2 stream behind a 22-byte header, so reading or writing one needs this
codec. These are the exact files BeSQLite compiles, copied from the iTwin native source:

    imodel-native/iModelCore/libsrc/compress/lzma/        → the seven .c files, Compiler.h, Precomp.h
    imodel-native/iModelCore/libsrc/compress/VendorAPI/liblzma/  → the headers


**Why this copy and not a fresh download from 7-zip.org.** Igor Pavlov's SDK is public domain and
every release is available, but the encoder's output is not stable across versions: a newer SDK
packs the same bytes differently. These files are dated 2009–2015 (`Lzma2Enc.c` is 2010-09-24,
≈ SDK 9.20), and using them means a changeset this tool rewrites is byte-identical to one BeSQLite's
own writer would have produced from the same stream — which is what makes `cmp` against BeSQLite's
output a usable acceptance test. A changeset's id is computed over *decompressed* bytes, so a
different encoder would still have been correct; it would just have been unverifiable this way.

These are iTwin's files as iTwin has them, which is not quite the same as Igor Pavlov's: four carry
Bentley edits, all marked as such in the source and none of them touching the codec —
`Lzma2Enc.h` gives `CLzma2EncProps` a struct tag so it can be forward-declared, `LzmaEnc.c` comments out a
`pp = pp;` self-assignment in the single-threaded branch of `LzmaEnc_Finish`, and `Alloc.c` and
`Alloc.h` add a `BENTLEY_WINRT` guard on Windows. Taking the files as they are is the point:
they are what produced the changesets this tool has to match.

The compression parameters BeSQLite passes (16 MB dictionary, level 7, no random-access blocking)
are reproduced in `src/Lzma.cpp`, not here.
