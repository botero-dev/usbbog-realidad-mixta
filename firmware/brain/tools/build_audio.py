#!/usr/bin/env python3
"""Build the firmware's embedded audio: source files -> C++ arrays.

WHAT IT DOES
------------
For every clip in SOURCES below it

  1. decodes/resamples the source in `audio/` to MONO signed-16-bit PCM at RATE
     (ffmpeg), writing the intermediate to `audio/pcm/<slug>.pcm`;
  2. emits the C++ the firmware compiles:
        include/audio_clips.h        the AudioClip struct, the rate, the externs
        src/audio_clips.cpp          the kAudioClips[] table
        src/audio_clip_<slug>.cpp    the samples, one file per clip

WHY IT RESAMPLES, AND WHY RATE IS 22050
--------------------------------------
These arrays are `.rodata`: they travel inside firmware.bin, and firmware.bin has
to fit in the app partition. On this board that partition is **3,342,336 bytes**
(Arduino's `default_8MB.csv`, which the Freenove board def selects -- see
platformio.ini) and the app itself already takes ~1.2 MB of it.

Cost is `seconds x RATE x 2 bytes`, so the rate IS the size knob:

      7 clips, 29.4 s total   44100 Hz -> 2.59 MB   does not fit
                              22050 Hz -> 1.30 MB   fits, ~1.2 MB spare
                              16000 Hz -> 0.94 MB   fits, telephone quality

22050 Hz carries 11 kHz of bandwidth, which is transparent for speech and is a
good trade for these clips. Raise RATE to 44100 for music at the cost of double
the flash.

Nothing is resampled ON the device: the I2S port runs at RATE too (`SAMPLE_RATE`
in `audio_player.cpp`), so the samples play back at their native rate and the DAC
just clocks slower. The generated header carries `AUDIO_CLIP_RATE` and
`audio_player.cpp` `static_assert`s it against the driver's rate, because the
failure mode otherwise is silent -- every clip plays at the wrong pitch and
nothing anywhere reports it.

WHY C++ ARRAYS AND NOT THE LINKER'S BINARY-EMBED SYMBOLS
-------------------------------------------------------
PlatformIO's `board_build.embed_files` works, but objcopy derives its symbols from
the ABSOLUTE path it is handed, so the symbol for `audio/clip.pcm` becomes
`_binary__home_<user>_..._audio_clip_pcm_start` -- a name that changes the moment
the project is moved or cloned. An array has no such problem.

WHY THE SOURCE FILES ARE MATCHED BY SUBSTRING
---------------------------------------------
Several of the masters have non-ASCII names (`"Soy más rápido que rápido"` uses a
combining accent, `"Una ganadora …. 42 perdedores"` an ellipsis), and Unicode
normalisation differs between filesystems. Matching each entry against the
directory listing by an ASCII-only substring avoids having to get the encoding of
a literal right in this file. Each matcher must hit EXACTLY ONE file -- ambiguity
is an error, not a guess.

ADDING A CLIP
-------------
1. Drop the file in `audio/`.
2. Add a line to SOURCES. The slug is what `/play?src=<slug>` accepts and what
   the buttons on the page send, so keep it short, ASCII and stable.
3. Re-run:  python3 tools/build_audio.py

USAGE
-----
    python3 tools/build_audio.py            # convert + regenerate the C++
    python3 tools/build_audio.py --check    # convert, report, write nothing
"""

import os
import struct
import subprocess
import sys
import tempfile

# --- the one knob ----------------------------------------------------------
RATE = 22050

# --- sources ---------------------------------------------------------------
# slug      what /play?src=.. accepts; also the C++ identifier pcm_<slug>
# match     ASCII-only substring that identifies ONE file in audio/
# src_rate  / src_channels: only for a bare .pcm, which has no header to read
SOURCES = [
    # The original clip. Kept because it is what the project was verified with;
    # its master is raw PCM, so the input rate has to be spelled out.
    {"slug": "clip", "match": "clip.pcm", "src_rate": 44100, "src_channels": 1},

    {"slug": "kachao1", "match": "Kachao I.m4a.mp4"},
    {"slug": "kachao2", "match": "Kachao II.m4a.mp4"},
    {"slug": "kachao3", "match": "Kachao III.m4a.mp4"},
    {"slug": "rapido", "match": "Soy m"},  # "Soy más rápido que rápido"
    {"slug": "ganadora", "match": "Una ganadora"},  # "Una ganadora …. 42 perdedores"
    {"slug": "velocidad", "match": "Velocidad, yo soy la velocidad"},
]

PER_LINE = 16


def project_root():
    """firmware/brain -- two levels up from tools/."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


def find_source(audio_dir, needle, taken):
    """The one file in audio_dir whose name contains `needle`.

    ASCII-only matching, because the real names are not: see the module docstring.
    """
    hits = sorted(f for f in os.listdir(audio_dir)
                  if needle in f and os.path.isfile(os.path.join(audio_dir, f)))
    hits = [h for h in hits if h not in taken]
    if not hits:
        raise SystemExit("no file in %s matches %r" % (audio_dir, needle))
    if len(hits) > 1:
        raise SystemExit("%r is ambiguous in %s: %s" % (needle, audio_dir, hits))
    return hits[0]


def convert(src_path, dst_path, src_rate=None, src_channels=None):
    """ffmpeg -> mono s16le PCM at RATE. Returns the sample count."""
    cmd = ["ffmpeg", "-y", "-v", "error"]
    if src_rate is not None:
        # A bare .pcm has no header, so size/rate/format must be stated up front.
        cmd += ["-f", "s16le", "-ar", str(src_rate), "-ac", str(src_channels or 1)]
    cmd += ["-i", src_path, "-ac", "1", "-ar", str(RATE),
            "-sample_fmt", "s16", "-f", "s16le", dst_path]

    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    subprocess.run(cmd, check=True)

    with open(dst_path, "rb") as fh:
        raw = fh.read()
    if len(raw) % 2:
        print("warning: %s is not a whole number of 16-bit samples" % dst_path)
    return len(raw) // 2


def read_samples(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    return struct.unpack("<%dh" % (len(raw) // 2), raw[: len(raw) & ~1])


def write_header(path, clips):
    with open(path, "w") as fh:
        fh.write(
            "// audio_clips.h -- GENERATED by tools/build_audio.py. Do not edit.\n"
            "//\n"
            "// Every clip the firmware can play: MONO signed 16-bit PCM at\n"
            "// AUDIO_CLIP_RATE Hz. Regenerate with `python3 tools/build_audio.py`\n"
            "// after adding a file to SOURCES there; read that script for why the\n"
            "// rate is what it is (it is the flash-size knob).\n"
            "#pragma once\n"
            "\n"
            "#include <stdint.h>\n"
            "\n"
            "// The rate every clip below is stored at. audio_player.cpp asserts this\n"
            "// against the I2S driver's own rate: the two MUST agree, or everything\n"
            "// plays at the wrong pitch and nothing reports it.\n"
            "constexpr uint32_t AUDIO_CLIP_RATE = %du;\n"
            "\n"
            "// One playable clip. `name` is what /play?src=<name> accepts and what\n"
            "// /status reports as `audio` while it plays.\n"
            "struct AudioClip {\n"
            "  const char *name;\n"
            "  const int16_t *pcm;   // mono, AUDIO_CLIP_RATE Hz\n"
            "  unsigned count;       // samples\n"
            "  uint32_t rate;        // Hz -- always AUDIO_CLIP_RATE\n"
            "};\n"
            "\n"
            "// The sample data. One array per clip, each defined in its own\n"
            "// src/audio_clip_<name>.cpp so that one big file cannot dominate the\n"
            "// build, and declared here so the table can reference them.\n"
            % RATE
        )
        for c in clips:
            fh.write("extern const int16_t pcm_%s[%uu];\n" % (c["slug"], c["count"]))
        fh.write(
            "\n"
            "// The table, in the order tools/build_audio.py lists the clips. That\n"
            "// order is also the order the page shows them in.\n"
            "extern const AudioClip kAudioClips[];\n"
            "extern const unsigned kAudioClipCount;\n"
        )


def write_table(path, clips):
    with open(path, "w") as fh:
        fh.write("// GENERATED by tools/build_audio.py -- do not edit.\n")
        fh.write("//\n")
        fh.write("// The clip table only. The samples themselves live in\n")
        fh.write("// src/audio_clip_<name>.cpp, one file each.\n")
        fh.write('#include "audio_clips.h"\n\n')
        fh.write("const AudioClip kAudioClips[] = {\n")
        for c in clips:
            fh.write('    {"%s", pcm_%s, %uu, %uu},  // %.3f s\n'
                     % (c["slug"], c["slug"], c["count"], RATE,
                        c["count"] / float(RATE)))
        fh.write("};\n\n")
        fh.write("const unsigned kAudioClipCount = "
                 "sizeof(kAudioClips) / sizeof(kAudioClips[0]);\n")


def write_samples(path, slug, samples):
    with open(path, "w") as fh:
        fh.write("// GENERATED by tools/build_audio.py -- do not edit.\n")
        fh.write("//\n")
        fh.write("// %s: %u samples, %.3f s, mono 16-bit at %u Hz.\n"
                 % (slug, len(samples), len(samples) / float(RATE), RATE))
        fh.write("#include <stdint.h>\n\n")
        fh.write('#include "audio_clips.h"\n\n')
        fh.write("const int16_t pcm_%s[%uu] = {\n" % (slug, len(samples)))
        for i in range(0, len(samples), PER_LINE):
            fh.write("    " + ", ".join("%6d" % s
                                        for s in samples[i:i + PER_LINE]) + ",\n")
        fh.write("};\n")


def main(argv):
    check = "--check" in argv
    root = project_root()
    audio_dir = os.path.join(root, "audio")
    pcm_dir = os.path.join(audio_dir, "pcm")
    inc_dir = os.path.join(root, "include")
    src_dir = os.path.join(root, "src")

    clips = []
    taken = set()
    total_bytes = 0

    for spec in SOURCES:
        src_name = find_source(audio_dir, spec["match"], taken)
        taken.add(src_name)
        src_path = os.path.join(audio_dir, src_name)
        if check:
            # Still convert -- the point of the dry run is the SIZES, and those are
            # only real after ffmpeg has resampled -- but land it in the temp dir so
            # the repository is untouched.
            dst_path = os.path.join(tempfile.gettempdir(),
                                    "brain-audio-%s.pcm" % spec["slug"])
        else:
            dst_path = os.path.join(pcm_dir, spec["slug"] + ".pcm")

        count = convert(src_path, dst_path,
                        src_rate=spec.get("src_rate"),
                        src_channels=spec.get("src_channels"))
        total_bytes += count * 2
        print("  %-10s %-42s %7u samples  %6.3f s"
              % (spec["slug"], src_name, count, count / float(RATE)))
        clips.append({"slug": spec["slug"], "count": count})

    print("  %-10s %-42s %7u bytes  %.2f MB  (%u Hz mono 16-bit)"
          % ("TOTAL", "", total_bytes, total_bytes / (1024.0 * 1024.0), RATE))

    if check:
        print("--check: nothing written")
        return 0

    os.makedirs(inc_dir, exist_ok=True)
    write_header(os.path.join(inc_dir, "audio_clips.h"), clips)
    write_table(os.path.join(src_dir, "audio_clips.cpp"), clips)
    for c in clips:
        write_samples(os.path.join(src_dir, "audio_clip_%s.cpp" % c["slug"]),
                      c["slug"], read_samples(os.path.join(pcm_dir,
                                                           c["slug"] + ".pcm")))

    print("wrote include/audio_clips.h, src/audio_clips.cpp and %d arrays"
          % len(clips))
    print("      app partition on this board: 3342336 B -- keep an eye on it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
