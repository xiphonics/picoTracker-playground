#!/usr/bin/env python3

import sys


DX7_BULK_HEADER = bytes((0xF0, 0x43, 0x00, 0x09, 0x20, 0x00))
DX7_BULK_TRAILER_SIZE = 2
DX7_PATCH_SIZE = 128
DX7_PATCH_COUNT = 32
DX7_BULK_PAYLOAD_SIZE = DX7_PATCH_SIZE * DX7_PATCH_COUNT


def dx7_checksum(payload):
    """Calculate the checksum used by Yamaha DX7 bulk dumps."""
    return (-sum(payload)) & 0x7F


def parse_syx_file(filename):
    """Parse a DX7 SYX file and extract instrument patches."""
    with open(filename, "rb") as f:
        data = f.read()

    if not data.startswith(DX7_BULK_HEADER):
        print("Error: Not a DX7 32-voice bulk dump")
        return None

    if data[-1] != 0xF7:
        print("Warning: File doesn't end with SYSEX end marker")

    if len(data) < len(DX7_BULK_HEADER) + DX7_BULK_TRAILER_SIZE:
        print("Error: SYX file is truncated")
        return None

    patch_data = data[len(DX7_BULK_HEADER) : -DX7_BULK_TRAILER_SIZE]
    checksum = data[-2]

    if len(patch_data) != DX7_BULK_PAYLOAD_SIZE:
        print(
            f"Error: Expected {DX7_BULK_PAYLOAD_SIZE} bytes of patch data, "
            f"found {len(patch_data)}"
        )
        return None

    expected_checksum = dx7_checksum(patch_data)
    if checksum != expected_checksum:
        print(
            f"Warning: Checksum mismatch (file=0x{checksum:02X}, "
            f"expected=0x{expected_checksum:02X})"
        )

    patches = []
    for i in range(DX7_PATCH_COUNT):
        start = i * DX7_PATCH_SIZE
        end = start + DX7_PATCH_SIZE
        patch = list(patch_data[start:end])
        patches.append(patch)

    return patches


def generate_c_header(patches, output_filename):
    """Generate a C header file containing the patches."""
    with open(output_filename, "w") as f:
        f.write("/**\n")
        f.write(" * DX7 Patch Data\n")
        f.write(" * Automatically generated from SYX file\n")
        f.write(" */\n\n")
        f.write("#ifndef DX7_PATCHES_H\n")
        f.write("#define DX7_PATCHES_H\n\n")
        f.write("#include <stdint.h>\n\n")

        f.write(f"#define DX7_NUM_PATCHES {len(patches)}\n\n")

        f.write("// Each patch is 128 bytes of DX7 parameter data\n")
        f.write("static const uint8_t dx7_patches[DX7_NUM_PATCHES][128] = {\n")

        for i, patch in enumerate(patches):
            f.write("    { // Patch ")
            f.write(str(i))
            f.write("\n     ")

            for j, byte in enumerate(patch):
                if j % 16 == 0 and j > 0:
                    f.write("\n     ")
                f.write("0x{:02X}".format(byte))
                if j < len(patch) - 1:
                    f.write(", ")

            f.write("\n    }")
            if i < len(patches) - 1:
                f.write(",")
            f.write("\n")

        f.write("};\n\n")
        f.write("#endif // DX7_PATCHES_H\n")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 syx_to_header.py <input.syx> [output.h]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "dx7_patches.h"

    patches = parse_syx_file(input_file)
    if patches is None:
        sys.exit(1)

    print(f"Found {len(patches)} patches in {input_file}")
    generate_c_header(patches, output_file)
    print(f"Generated {output_file}")


if __name__ == "__main__":
    main()
