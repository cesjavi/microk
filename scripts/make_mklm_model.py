#!/usr/bin/env python3
import argparse
import struct

HEADER_SIZE = 72
MAGIC = 0x4D4C4B4D
VERSION = 1
MODEL_TYPE_RULES = 1
MODEL_TYPE_NN = 2
RULE_PAYLOAD_MAGIC = 0x50524B4D
NN_PAYLOAD_MAGIC = 0x4E4E4B4D


def words(text):
    out = []
    current = []
    for ch in text.lower():
        if ch.isalnum():
            current.append(ch)
        elif current:
            out.append("".join(current))
            current = []
    if current:
        out.append("".join(current))
    return out


def build_rule_payload(pairs):
    payload = struct.pack("<II", RULE_PAYLOAD_MAGIC, len(pairs))
    for prompt_bytes, response_bytes in pairs:
        payload += struct.pack("<HH", len(prompt_bytes), len(response_bytes))
        payload += prompt_bytes
        payload += response_bytes
    return MODEL_TYPE_RULES, payload


def build_nn_payload(text_pairs):
    tokenized = [(words(prompt), response) for prompt, response in text_pairs]
    vocab = []
    for prompt_words, _ in tokenized:
        for word in prompt_words:
            if word not in vocab:
                vocab.append(word)
    if not vocab:
        vocab = ["default"]
    if len(vocab) > 32:
        vocab = vocab[:32]

    payload = struct.pack("<HHI", len(vocab), len(text_pairs), 0)
    payload = struct.pack("<I", NN_PAYLOAD_MAGIC) + payload
    for word in vocab:
        encoded = word.encode("utf-8")
        payload += struct.pack("<H", len(encoded))
        payload += encoded

    for prompt, response in text_pairs:
        prompt_words = set(words(prompt))
        response_bytes = response.encode("utf-8")
        payload += struct.pack("<hH", 0, len(response_bytes))
        payload += response_bytes
        for word in vocab:
            payload += struct.pack("<h", 10 if word in prompt_words else -1)

    return MODEL_TYPE_NN, payload


def main():
    parser = argparse.ArgumentParser(description="Create a dummy MicroK MKLM v1 model")
    parser.add_argument("output")
    parser.add_argument("--name", default="dummy-mklm")
    parser.add_argument("--pair", action="append", default=[], help="Rule pair: prompt=response")
    parser.add_argument("--mode", choices=["rules", "nn"], default="nn")
    args = parser.parse_args()

    name = args.name.encode("ascii", "ignore")[:31]
    name = name + b"\0" * (32 - len(name))
    pairs = args.pair or [
        "hello=Hello from a MicroK model module.",
        "status=The MKLM rule model is loaded and answering from module memory.",
        "name=I am a tiny MKLM rule model.",
    ]

    text_pairs = []
    entries = []
    for pair in pairs:
        if "=" not in pair:
            raise SystemExit(f"Invalid --pair {pair!r}; expected prompt=response")
        prompt, response = pair.split("=", 1)
        text_pairs.append((prompt, response))
        prompt_bytes = prompt.encode("utf-8")
        response_bytes = response.encode("utf-8")
        if len(prompt_bytes) > 65535 or len(response_bytes) > 65535:
            raise SystemExit("Prompt/response too long for MKLM rule payload")
        entries.append((prompt_bytes, response_bytes))

    if args.mode == "rules":
        model_type, payload = build_rule_payload(entries)
    else:
        model_type, payload = build_nn_payload(text_pairs)

    header = struct.pack(
        "<IIIIIIIIII32s",
        MAGIC,
        VERSION,
        HEADER_SIZE,
        model_type,
        0,
        0,
        0,
        0,
        HEADER_SIZE,
        len(payload),
        name,
    )

    with open(args.output, "wb") as f:
        f.write(header)
        f.write(payload)


if __name__ == "__main__":
    main()
