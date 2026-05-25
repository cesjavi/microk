# MicroK LLM Model Format

MicroK can load an LLM model as a Multiboot module. The first supported
format is a tiny validation header named MKLM v1. The current backend supports
a simple rule-table payload and a tiny neural intent payload, then falls back
to mock responses.

## Header

All fields are little-endian.

```c
typedef struct {
    uint32_t magic;       // 0x4D4C4B4D, bytes "MKLM"
    uint32_t version;     // 1
    uint32_t header_size; // sizeof header, currently 72
    uint32_t model_type;  // 1 = rule table, 2 = neural intent model
    uint32_t vocab_size;  // reserved
    uint32_t hidden_size; // reserved
    uint32_t layer_count; // reserved
    uint32_t flags;       // reserved
    uint32_t data_offset; // offset to payload
    uint32_t data_size;   // payload byte size
    char name[32];        // zero-terminated when shorter than 32
} microk_model_header_t;
```

## GRUB

```cfg
menuentry "MicroK AI" {
    multiboot /boot/kernel.bin
    module /boot/initrd.img initrd
    module /boot/model.mklm llm model
    boot
}
```

## Rule Payload

When `model_type` is `1`, `data_offset` points to an MKRP rule payload:

```c
typedef struct {
    uint32_t magic;       // 0x50524B4D, bytes "MKRP"
    uint32_t entry_count;
} microk_rule_payload_t;

typedef struct {
    uint16_t prompt_len;
    uint16_t response_len;
    // prompt bytes follow, then response bytes
} microk_rule_entry_t;
```

The kernel checks each rule against the prompt using exact or substring match.

## Neural Intent Payload

When `model_type` is `2`, `data_offset` points to an MKNN payload:

```c
typedef struct {
    uint32_t magic;       // 0x4E4E4B4D, bytes "MKNN"
    uint16_t vocab_count; // max 32 used by current kernel
    uint16_t class_count;
    uint32_t reserved;
} microk_nn_payload_t;
```

Then come `vocab_count` vocabulary entries:

```c
typedef struct {
    uint16_t len;
    // len bytes follow
} microk_nn_vocab_entry_t;
```

Then come `class_count` classes:

```c
typedef struct {
    int16_t bias;
    uint16_t response_len;
    // response bytes follow
    // then vocab_count int16 weights
} microk_nn_class_entry_t;
```

At inference time, MicroK marks a vocab feature as active if the word appears in
the prompt. Each class score is:

```text
score = bias + sum(active_feature_weight)
```

The response from the highest positive score is returned.

## Runtime Metadata

Inside MicroK, `llm info` reports metadata parsed from the loaded model:

- model name from the MKLM header,
- module size in bytes,
- backend type,
- MKNN vocab/class counts, or MKRP rule count.

## Create a Test Model

```bash
python3 scripts/make_mklm_model.py model.mklm \
  --name test-model \
  --pair "hello=Hello from the USB model." \
  --pair "status=The model module is loaded."
```

The default mode is `--mode nn`. Use `--mode rules` to generate the older rule
table format.

## Generator Tests

Run:

```bash
make test-model
```

The test creates temporary MKNN and MKRP models and validates their headers,
payload magic values, counts, sizes, and name truncation.
