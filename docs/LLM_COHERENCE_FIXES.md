# LLM Generation Coherence: Fixes and Open Issues

Investigation triggered by testing `tinyllama-1.1b-chat-v1.0.Q4_0.gguf` for the
first time in QEMU (this build had never been boot-tested on this machine
before this session — see the `-mstackrealign` note in
`docs/PAE_MEMORY_PLAN.md`). Generation didn't crash and produced real English
words, but no coherent sentences, unlike `stories15M.gguf` which already
produced grammatical (if simple) output on the same code.

## Fixed

### 1. Missing BOS token

`tokenizer.ggml.bos_token_id` (`1` in both `stories15M.gguf` and
`tinyllama-1.1b-chat-v1.0.Q4_0.gguf`) was never read or fed to the model.
Every Llama-family model is trained with a leading BOS token on every
sequence; without it, generation starts from a state the model never saw in
training.

- `llm_get_bos_token_id()` added (`kernel/llm.c`), reads the real ID from GGUF
  metadata via `gguf_get_metadata_value(..., "tokenizer.ggml.bos_token_id",
  GGUF_METADATA_UINT32, ...)`, falls back to `1` if the key is missing.
- `llm_try_gguf_inference()` and `llm_dump_top_logits()` now run one extra
  forward-pass step for BOS at position 0, before the real prompt tokens.
- Verified in QEMU by counting `LLM ENERGY pre-layers` occurrences (one per
  forward pass): 17 total for a 14-token chat-template prompt + 2 generated
  tokens = 1 (BOS) + 14 (prompt) + 2 (generated), confirming BOS is actually
  fed through.

### 2. EOS token never checked during generation

`tokenizer.ggml.eos_token_id` (`2`) was never compared against the argmax
winner. The generation loop only stopped on `max_new_tokens`, timeout, or
cancellation — never because the model itself "wanted" to stop — forcing
continued output past the point a correct decoder would end the response.

- `llm_get_eos_token_id()` added, same metadata-read-with-fallback pattern as
  BOS (fallback `2`).
- `llm_output_logit_for_token()` added: computes a single vocab row's logit
  against the current hidden state, reusing the same fast Q4_0/Q6_K
  fused-dot-product paths as the main argmax scan. Needed because EOS is
  deliberately excluded from that scan (`llm_preview_token_usable()` rejects
  `<`-prefixed control tokens so they're never printed as if they were a
  generated word).
- In the generation loop, after the normal argmax scan, EOS's own logit is
  computed and compared against the best printable candidate's logit; if EOS
  wins, generation stops (`break`) without emitting it as text.

### 3. Tokenizer did not implement real BPE

`kernel/tokenizer.c`'s `tokenizer_encode()` used a greedy "find the longest
complete vocabulary entry starting at this position" heuristic. It never
loaded `tokenizer.ggml.scores` (`t->scores` was hardcoded `NULL`, "optional,
can load if needed" and never was), so it had no way to reproduce the actual
merge priority the model was trained with. For most English words already
present whole in the ~32k vocab this coincidentally matches real BPE, but for
anything else (foreign words, rare compounds) it can diverge from the exact
token sequence the model actually learned to interpret.

Rewrote `kernel/tokenizer.c`:

- `tokenizer_create()` now also loads `tokenizer.ggml.scores` (one f32 per
  vocab entry) and caches each vocab string's length in `t->lengths`
  (`tokenizer_t` gained a `lengths` field in `kernel/tokenizer.h`) to avoid
  recomputing `strlen()` on every merge-candidate lookup.
- `tokenizer_encode()` now implements the actual SentencePiece/Llama BPE
  algorithm (the one llama.cpp runs for `tokenizer.ggml.model=llama`): split
  the (space-marker-preprocessed) prompt into one symbol per UTF-8 character,
  then repeatedly merge the adjacent pair whose concatenation is itself a
  vocab entry and has the highest `tokenizer.ggml.scores` value, until no
  mergeable pair remains. Symbols are offset+length slices of the same
  buffer (adjacent symbols are always contiguous), so merging never copies —
  just extends a length and splices a linked list.
- Final symbols that never got merged down to a whole vocab entry (rare —
  e.g. an unusual character with no learned single-token form) fall back to
  per-byte `<0xXX>` byte-fallback tokens instead of being silently dropped
  (the old behavior on any non-match: `ptr++ // skip unknown char`).
- Note: `tokenizer.ggml.merges` (61249 entries in the TinyLlama file) is the
  GPT2-style explicit-merge-list format and is *not* what llama.cpp actually
  uses for `tokenizer.ggml.model=llama` — it uses the score-based algorithm
  above instead. The merges array is present in some GGUF exports for
  compatibility but deliberately unused here, matching llama.cpp's own loader
  behavior.

**Verified**: `stories15M` still passes `llm selftest gguf`, no regression,
same-quality output (expected — `"the"` is already a single vocab token, so
old and new algorithms agree on it). For TinyLlama + `"hola"`, the new BPE
tokenizer produced the *same* split (`▁hol` + `a`) as the old greedy
heuristic — not a bug, just the expected outcome when the longest candidate
also happens to have the best score.

## Still broken (not fixed by the above)

A ~9-minute generation run (`llm chat hola`, TinyLlama, all three fixes
above applied) produced:

```
Californenglisch rid During primer &= coreskiego Viroothlocalhost experiment extractlevantinternalProvider
```

16 tokens, no grammatical structure, mixed-language fragments
(English/German/Spanish/Polish-looking suffixes) and programming jargon
(`localhost`, `Provider`, `experiment`) — arguably worse-looking than before
BOS/EOS/BPE, and EOS never won the comparison once in 16 tokens.

**Conclusion**: BOS, EOS, and real BPE were all real, verified, worthwhile
fixes, but none of them was the dominant cause of TinyLlama's incoherence.
Since `stories15M` (plain MHA, F32/Q4_0 weights) produces genuinely
grammatical output on the exact same code path, and TinyLlama (GQA — 32 Q
heads / 4 KV heads — and a Q6_K output projection) does not, the likely
remaining cause is a silent numerical bug specific to one of those two
model-specific code paths:

- Grouped-Query Attention head mapping/KV-cache striding
  (`llm_effective_n_kv_head`, `kv_dim`, `group_size` in
  `run_llama_forward_pass`, `kernel/llm.c`).
- The Q6_K fused dot product used for the output projection
  (`dot_product_q6_k_float`, `kernel/llm.c`).

Both are already marked "fixed" in `ROADMAP.md` (Etapa 6), but that
verification only confirmed "doesn't crash / no numeric overflow in the
energy trace" — never "matches a known-good reference implementation's
numbers for the same weights." That's the gap.

### Suggested next step (not started)

Compare intermediate values (attention output for a single head, or the Q6_K
dot product for a few output rows) against a reference implementation
(llama.cpp or a small Python script using the same GGUF weights) for the
exact same input, layer by layer, to find where TinyLlama's numbers actually
diverge from correct. This is a different, larger investigation than the
tokenizer/BOS/EOS work above — not a quick follow-up patch.
