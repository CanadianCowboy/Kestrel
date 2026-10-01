#!/usr/bin/env python3
"""Find the rendered text bands in a screenshot, without any image library.

`tools/verify-desktop.sh` runs this over the window capture to answer one
question: did the conversation actually draw anything? A window can be a
correct size, correctly laid out, and completely empty, and every other
measurement in that script passes on such a window. This is the check that
catches it.

It exists as its own file rather than as a line in the shell script because the
answer needs a PNG decoder, and the point of the exercise was to avoid
depending on one. The venv that runs the voice has no Pillow, the packaged app
is not allowed a build-time Python, and `pip install pillow` would put a
package in the distribution purely to service a test. So: zlib is in the
standard library, and a PNG is a zlib stream wrapped in a handful of chunks.

    python tools/pngrows.py shot.png [--column 0.30 0.86] [--min-luma 24]

Prints the image size, the bands, and how many rows are darker than the
background -- the last of which is what a "the transcript is an empty black
rectangle" regression looks like from the outside.
"""

import struct
import sys
import zlib


def read_png(path):
    """Return (width, height, channels, rows) for a non-interlaced PNG."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)

    offset = 8
    width = height = depth = colour = interlace = None
    compressed = bytearray()
    while offset < len(data):
        (length,) = struct.unpack_from(">I", data, offset)
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length                      # length + type + body + crc

        if kind == b"IHDR":
            (width, height, depth, colour, _comp,
             _filt, interlace) = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break

    if depth != 8:
        raise ValueError("only 8-bit PNGs are handled, this is %d-bit" % depth)
    if interlace:
        raise ValueError("interlaced PNGs are not handled")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(bytes(compressed))

    stride = width * channels
    rows = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        # The five PNG filters, per the spec. Undo them in place; `previous`
        # is the already-reconstructed row above.
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up = previous[i]
                upleft = previous[i - channels] if i >= channels else 0
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                if pa <= pb and pa <= pc:
                    pred = left
                elif pb <= pc:
                    pred = up
                else:
                    pred = upleft
                line[i] = (line[i] + pred) & 0xFF
        elif filter_type != 0:
            raise ValueError("unknown PNG filter %d" % filter_type)
        rows.append(line)
        previous = line
    return width, height, channels, rows


def luma(row, channels, x):
    i = x * channels
    if channels <= 2:
        return row[i]
    return (row[i] * 299 + row[i + 1] * 587 + row[i + 2] * 114) // 1000


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    low, high = 0.30, 0.86
    minimum = 24
    if "--column" in argv:
        i = argv.index("--column")
        low, high = float(argv[i + 1]), float(argv[i + 2])
    if "--min-luma" in argv:
        minimum = int(argv[argv.index("--min-luma") + 1])

    width, height, channels, rows = read_png(path)
    x0, x1 = int(width * low), int(width * high)

    # One number per row: the brightest pixel in the transcript column. Text is
    # the only thing in that column that is much lighter than the background, so
    # a row belongs to a band when its peak clears the threshold.
    peaks = []
    for y in range(height):
        row = rows[y]
        peaks.append(max(luma(row, channels, x) for x in range(x0, x1)))

    bands = []
    start = None
    for y in range(height):
        if peaks[y] >= minimum:
            if start is None:
                start = y
        elif start is not None:
            bands.append((start, y - 1))
            start = None
    if start is not None:
        bands.append((start, height - 1))

    print("%dx%d, %d channels" % (width, height, channels))
    print("rendered text bands in the transcript column:")
    for a, b in bands:
        print("  y=%d..%d  height=%d  brightest=%d"
              % (a, b, b - a + 1, max(peaks[a:b + 1])))
    if not bands:
        print("  NONE -- nothing was drawn in the transcript column")

    # A run of near-black rows is what an invisible transcript looks like: the
    # rows are laid out at the right heights and filled with nothing.
    dark = sum(1 for y in range(height) if peaks[y] <= 4)
    print("rows darker than luma 4 (the old black band): %d" % dark)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
