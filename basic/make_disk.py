#!/usr/bin/env python3
"""
make_disk.py  --  Create a 720 KB FAT12 disk image pre-loaded with BASIC
                  example programs and a HELP.TXT reference file.

Run from the repository root:
    python3 basic/make_disk.py [output.img]

Default output: basic_demo.img
"""

import os
import struct
import subprocess
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT  = os.path.dirname(SCRIPT_DIR)
MSDOS_BIN  = os.path.join(REPO_ROOT, 'msdos')

FILES = [
    'HELP.TXT',
    'CALC.BAS',
    'GUESS.BAS',
    'HAMURABI.BAS',
    'TIMES.BAS',
    'FIB.BAS',
]

# ---------------------------------------------------------------------------
# FAT12 helpers
# ---------------------------------------------------------------------------

class Fat12Image:
    def __init__(self, path):
        with open(path, 'r+b') as f:
            self.data = bytearray(f.read())
        self.path = path
        self._parse_bpb()

    def _parse_bpb(self):
        d = self.data
        self.bytes_per_sec = struct.unpack_from('<H', d, 11)[0]
        self.secs_per_clus = d[13]
        self.reserved_secs = struct.unpack_from('<H', d, 14)[0]
        self.num_fats      = d[16]
        self.root_entries  = struct.unpack_from('<H', d, 17)[0]
        self.fat_size      = struct.unpack_from('<H', d, 22)[0]  # secs per FAT

        bps = self.bytes_per_sec
        self.fat1_off  = self.reserved_secs * bps
        self.fat2_off  = self.fat1_off + self.fat_size * bps
        self.root_off  = self.fat2_off + self.fat_size * bps
        self.data_off  = self.root_off + self.root_entries * 32
        self.clus_size = self.secs_per_clus * bps
        self.total_clusters = (len(self.data) - self.data_off) // self.clus_size
        self._root_idx = 0   # next free root directory slot

        print(f"  BPB: {bps} bytes/sec, {self.secs_per_clus} sec/clus, "
              f"{self.fat_size} FAT secs, {self.root_entries} root entries")
        print(f"  FAT at {self.fat1_off:#x}, root at {self.root_off:#x}, "
              f"data at {self.data_off:#x}, {self.total_clusters} clusters")

    # --- FAT entry read/write (updates both copies) -------------------------

    def fat_get(self, n):
        off = self.fat1_off + n + n // 2
        word = self.data[off] | (self.data[off + 1] << 8)
        return (word >> 4) if (n & 1) else (word & 0x0FFF)

    def fat_set(self, n, val):
        for base in (self.fat1_off, self.fat2_off):
            off  = base + n + n // 2
            word = self.data[off] | (self.data[off + 1] << 8)
            if n & 1:
                word = (word & 0x000F) | ((val & 0x0FFF) << 4)
            else:
                word = (word & 0xF000) | (val & 0x0FFF)
            self.data[off]     = word & 0xFF
            self.data[off + 1] = (word >> 8) & 0xFF

    # --- Cluster allocation -------------------------------------------------

    def alloc_cluster(self, prev=None):
        """Find a free cluster, mark it EOF, optionally chain from prev."""
        for c in range(2, self.total_clusters + 2):
            if self.fat_get(c) == 0x000:
                self.fat_set(c, 0xFFF)     # EOF marker
                if prev is not None:
                    self.fat_set(prev, c)  # link chain
                return c
        raise RuntimeError("Disk full")

    def write_cluster(self, clus, data, offset=0):
        dst = self.data_off + (clus - 2) * self.clus_size
        chunk = data[offset : offset + self.clus_size]
        self.data[dst : dst + len(chunk)] = chunk

    # --- Root directory entry -----------------------------------------------

    def write_root_entry(self, name8, ext3, first_clus, file_size):
        if self._root_idx >= self.root_entries:
            raise RuntimeError("Root directory full")
        off = self.root_off + self._root_idx * 32
        # 8-char name, 3-char ext (space-padded, upper-case)
        n = name8.upper().encode('ascii').ljust(8)[:8]
        e = ext3.upper().encode('ascii').ljust(3)[:3]
        self.data[off:off+8]   = n
        self.data[off+8:off+11] = e
        self.data[off+11] = 0x20          # ARCHIVE attribute
        self.data[off+12:off+26] = b'\x00' * 14
        struct.pack_into('<H', self.data, off + 26, first_clus)
        struct.pack_into('<I', self.data, off + 28, file_size)
        self._root_idx += 1

    # --- High-level: write a file -------------------------------------------

    def write_file(self, filename, data):
        if '.' in filename:
            name, ext = filename.rsplit('.', 1)
        else:
            name, ext = filename, ''
        size = len(data)
        if size == 0:
            self.write_root_entry(name, ext, 0, 0)
            return

        first_clus = None
        prev_clus  = None
        offset     = 0
        while offset < size:
            clus = self.alloc_cluster(prev=prev_clus)
            if first_clus is None:
                first_clus = clus
            self.write_cluster(clus, data, offset)
            prev_clus = clus
            offset   += self.clus_size

        self.write_root_entry(name, ext, first_clus, size)
        print(f"  {filename:<14}  {size:>5} bytes  (cluster {first_clus})")

    def save(self):
        with open(self.path, 'wb') as f:
            f.write(self.data)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    out_img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO_ROOT, 'basic_demo.img')

    if not os.path.isfile(MSDOS_BIN):
        print(f"ERROR: {MSDOS_BIN} not found. Run 'make' first.")
        sys.exit(1)

    # 1. Format the image
    print(f"Formatting {out_img} ...")
    result = subprocess.run(
        [MSDOS_BIN, '--format', out_img, '--720'],
        capture_output=True, text=True)
    print(result.stderr.strip())
    if result.returncode != 0:
        print("Format failed:", result.stdout)
        sys.exit(1)

    # 2. Open image and write files
    img = Fat12Image(out_img)
    print(f"\nWriting files from {SCRIPT_DIR}:")

    for filename in FILES:
        src = os.path.join(SCRIPT_DIR, filename)
        if not os.path.isfile(src):
            print(f"  WARNING: {src} not found, skipping.")
            continue
        with open(src, 'rb') as f:
            raw = f.read()
        # Normalise to DOS line endings (CRLF) for text files
        if filename.endswith(('.TXT', '.BAS')):
            text = raw.decode('ascii', errors='replace')
            text = text.replace('\r\n', '\n').replace('\r', '\n')
            raw  = text.replace('\n', '\r\n').encode('ascii', errors='replace')
        img.write_file(filename, raw)

    img.save()

    # 3. Quick sanity-check via DIR
    print(f"\nDirectory listing of {out_img}:")
    check = subprocess.run(
        ['bash', '-c', f'printf "DIR\\nEXIT\\n" | {MSDOS_BIN} {out_img}'],
        capture_output=True, text=True)
    # Print only the DIR output lines
    for line in check.stdout.splitlines():
        print(f"  {line}")

    print(f"\nDone. Boot the image with:  ./msdos {out_img}")
    print( "Then run programs with e.g.: BASIC HAMURABI.BAS")

if __name__ == '__main__':
    main()
