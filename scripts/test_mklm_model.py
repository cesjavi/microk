#!/usr/bin/env python3
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "make_mklm_model.py"
HEADER = "<IIIIIIIIII32s"
HEADER_SIZE = 72
MAGIC = 0x4D4C4B4D
VERSION = 1
MODEL_TYPE_RULES = 1
MODEL_TYPE_NN = 2
RULE_PAYLOAD_MAGIC = 0x50524B4D
NN_PAYLOAD_MAGIC = 0x4E4E4B4D


def run_generator(output, *args):
    cmd = [sys.executable, str(SCRIPT), str(output), *args]
    subprocess.run(cmd, check=True, cwd=ROOT)


def read_model(path):
    data = path.read_bytes()
    assert len(data) >= HEADER_SIZE
    header = struct.unpack_from(HEADER, data, 0)
    keys = [
        "magic",
        "version",
        "header_size",
        "model_type",
        "vocab_size",
        "hidden_size",
        "layer_count",
        "flags",
        "data_offset",
        "data_size",
        "name",
    ]
    return dict(zip(keys, header)), data


def clean_name(raw):
    return raw.split(b"\0", 1)[0].decode("ascii")


def assert_header(header, data, model_type, name):
    assert header["magic"] == MAGIC
    assert header["version"] == VERSION
    assert header["header_size"] == HEADER_SIZE
    assert header["model_type"] == model_type
    assert header["data_offset"] == HEADER_SIZE
    assert header["data_size"] == len(data) - HEADER_SIZE
    assert clean_name(header["name"]) == name[:31]


def test_rules(tmp):
    output = tmp / "rules.mklm"
    run_generator(
        output,
        "--mode",
        "rules",
        "--name",
        "rules-test",
        "--pair",
        "hello=Hello from rules.",
        "--pair",
        "status=Rules active.",
    )
    header, data = read_model(output)
    assert_header(header, data, MODEL_TYPE_RULES, "rules-test")

    payload = data[header["data_offset"] :]
    magic, entry_count = struct.unpack_from("<II", payload, 0)
    assert magic == RULE_PAYLOAD_MAGIC
    assert entry_count == 2

    cursor = 8
    prompt_len, response_len = struct.unpack_from("<HH", payload, cursor)
    cursor += 4
    assert payload[cursor : cursor + prompt_len] == b"hello"
    cursor += prompt_len
    assert payload[cursor : cursor + response_len] == b"Hello from rules."


def test_nn(tmp):
    output = tmp / "nn.mklm"
    run_generator(
        output,
        "--mode",
        "nn",
        "--name",
        "nn-test",
        "--pair",
        "hola estado=Estado OK.",
        "--pair",
        "kernel ayuda=Ayuda kernel.",
    )
    header, data = read_model(output)
    assert_header(header, data, MODEL_TYPE_NN, "nn-test")

    payload = data[header["data_offset"] :]
    magic, vocab_count, class_count, reserved = struct.unpack_from("<IHHI", payload, 0)
    assert magic == NN_PAYLOAD_MAGIC
    assert reserved == 0
    assert vocab_count == 4
    assert class_count == 2


def test_name_truncation(tmp):
    output = tmp / "long-name.mklm"
    long_name = "abcdefghijklmnopqrstuvwxyz0123456789"
    run_generator(output, "--name", long_name, "--pair", "x=y")
    header, data = read_model(output)
    assert_header(header, data, MODEL_TYPE_NN, long_name)
    assert len(clean_name(header["name"])) == 31


def main():
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        test_rules(tmp)
        test_nn(tmp)
        test_name_truncation(tmp)
    print("MKLM model generator tests: OK")


if __name__ == "__main__":
    main()
