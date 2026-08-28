#!/usr/bin/env python3
"""Decrypt a XEX2 basefile so its contents can be scanned.

XenosRecomp finds shaders by scanning raw bytes for a container header, but a
retail default.xex is encrypted, so scanning the file as it sits on disc finds
nothing. That is why the generated shader cache comes out empty. This writes the
decrypted image, which XenosRecomp can then read like any other file.

    python tools/dump_xex_image.py assets/default.xex -o assets/default.xex.image

Blue Dragon's basefile is AES encrypted but *not* compressed, so no LZX
decompression is involved. If a title turns out to be compressed this will say
so and stop rather than writing something plausible-looking and wrong.

The correctness check is free and exact: a XEX basefile is a PE image, so a
successful decrypt begins with "MZ". A wrong key produces noise, not an "MZ".
"""
import argparse
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# XEX2 optional header keys.
XEX_FILE_FORMAT_INFO = 0x000003FF

# The per-title session key is itself encrypted with one of these. Retail discs
# use the first; a devkit build uses all zeroes.
KEYS = {
    "retail": bytes.fromhex("20B185A59D28FDC340583FBB0896BF91"),
    "devkit": bytes(16),
}

# Offset of the AES session key inside xex2_security_info.
SECURITY_INFO_AES_KEY = 0x150

ENCRYPTION = {0: "none", 1: "AES"}
COMPRESSION = {1: "none", 2: "basic", 3: "normal (LZX)", 4: "delta"}


def parse(data):
    if data[:4] != b"XEX2":
        sys.exit("not a XEX2 file (no XEX2 magic)")
    pe_offset, _reserved, security_offset, header_count = struct.unpack_from(">IIII", data, 8)

    headers = {}
    for i in range(header_count):
        key, value = struct.unpack_from(">II", data, 24 + i * 8)
        headers[key] = value

    if XEX_FILE_FORMAT_INFO not in headers:
        sys.exit("no file format info header; cannot tell how the basefile is stored")
    off = headers[XEX_FILE_FORMAT_INFO]
    _size, encryption, compression = struct.unpack_from(">IHH", data, off)
    return pe_offset, security_offset, encryption, compression


def decrypt(data, pe_offset, security_offset, key_name):
    encrypted_session_key = data[security_offset + SECURITY_INFO_AES_KEY:
                                 security_offset + SECURITY_INFO_AES_KEY + 16]

    # The session key is stored encrypted under the platform key, ECB, one block.
    unwrap = Cipher(algorithms.AES(KEYS[key_name]), modes.ECB()).decryptor()
    session_key = unwrap.update(encrypted_session_key) + unwrap.finalize()

    body = data[pe_offset:]
    # CBC with a zero IV over the whole basefile. Trailing bytes that do not
    # fill a block are stored in the clear.
    usable = len(body) - (len(body) % 16)
    cipher = Cipher(algorithms.AES(session_key), modes.CBC(bytes(16))).decryptor()
    return cipher.update(body[:usable]) + cipher.finalize() + body[usable:], session_key


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xex", help="path to default.xex")
    ap.add_argument("-o", "--output", default="assets/default.xex.image")
    ap.add_argument("--key", choices=sorted(KEYS) + ["auto"], default="auto")
    args = ap.parse_args()

    data = open(args.xex, "rb").read()
    pe_offset, security_offset, encryption, compression = parse(data)
    print("basefile at 0x%X, security info at 0x%X" % (pe_offset, security_offset))
    print("encryption: %s, compression: %s"
          % (ENCRYPTION.get(encryption, "?"), COMPRESSION.get(compression, "?")))

    if compression != 1:
        sys.exit("basefile is %s-compressed; this tool only handles uncompressed "
                 "basefiles, decompression would need the SDK's LZX support"
                 % COMPRESSION.get(compression, compression))

    if encryption == 0:
        image = data[pe_offset:]
        print("not encrypted; copying the basefile out as-is")
    else:
        candidates = sorted(KEYS) if args.key == "auto" else [args.key]
        for name in candidates:
            image, session_key = decrypt(data, pe_offset, security_offset, name)
            if image[:2] == b"MZ":
                print("decrypted with the %s key (session key %s)"
                      % (name, session_key.hex()))
                break
            print("  %s key gives %r, not MZ" % (name, image[:2]))
        else:
            sys.exit("no key produced a PE image; the basefile may use a scheme "
                     "this tool does not implement")

    with open(args.output, "wb") as fh:
        fh.write(image)
    print("wrote %s (%d bytes)" % (args.output, len(image)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
