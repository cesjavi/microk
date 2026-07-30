#!/usr/bin/env python3
"""Minimal standalone GGUF tensor-type inspector (no external deps).

Reads the GGUF header + metadata + tensor-info sections directly and
prints each tensor's name and ggml type, so we can see exactly which
tensors aren't Q4_0/F16/F32 in a given file without needing the kernel
or any GGUF python library installed.
"""
import struct
import sys

GGML_TYPE_NAMES = {
    0: "F32", 1: "F16", 2: "Q4_0", 3: "Q4_1", 6: "Q5_0", 7: "Q5_1",
    8: "Q8_0", 9: "Q8_1", 10: "Q2_K", 11: "Q3_K", 12: "Q4_K", 13: "Q5_K",
    14: "Q6_K", 15: "Q8_K", 16: "IQ2_XXS", 24: "I8", 25: "I16", 26: "I32",
}

GGUF_TYPE_SIZES = {
    0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 8: None, 9: None, 10: 8, 11: 8, 12: 8,
}


def read_str(f):
    (length,) = struct.unpack("<Q", f.read(8))
    return f.read(length).decode("utf-8", "replace")


def skip_value(f, vtype):
    if vtype in (0, 1, 7):
        f.read(1)
    elif vtype in (2, 3):
        f.read(2)
    elif vtype in (4, 5, 6):
        f.read(4)
    elif vtype in (10, 11, 12):
        f.read(8)
    elif vtype == 8:
        read_str(f)
    elif vtype == 9:
        (sub_type,) = struct.unpack("<I", f.read(4))
        (count,) = struct.unpack("<Q", f.read(8))
        for _ in range(count):
            skip_value(f, sub_type)
    else:
        raise ValueError(f"unknown metadata type {vtype}")


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"GGUF":
            print("not a GGUF file")
            return
        (version,) = struct.unpack("<I", f.read(4))
        (tensor_count,) = struct.unpack("<Q", f.read(8))
        (metadata_count,) = struct.unpack("<Q", f.read(8))
        print(f"version={version} tensor_count={tensor_count} metadata_count={metadata_count}")

        for _ in range(metadata_count):
            key = read_str(f)
            (vtype,) = struct.unpack("<I", f.read(4))
            skip_value(f, vtype)

        print("\n--- tensors ---")
        for _ in range(tensor_count):
            name = read_str(f)
            (ndims,) = struct.unpack("<I", f.read(4))
            dims = struct.unpack(f"<{ndims}Q", f.read(8 * ndims))
            (ttype,) = struct.unpack("<I", f.read(4))
            (offset,) = struct.unpack("<Q", f.read(8))
            type_name = GGML_TYPE_NAMES.get(ttype, f"UNKNOWN({ttype})")
            interesting = name in ("output.weight", "token_embd.weight", "output_norm.weight") or "blk.0." in name
            marker = "  <--" if interesting else ""
            print(f"{name:40s} dims={dims} type={ttype}:{type_name} offset={offset}{marker}")


if __name__ == "__main__":
    main()
