# LLM Boot Test

This repo includes two model files for testing:

- `models/model.mklm`: MicroK rule model understood by the current kernel.
- `models/tin1.gguf`: GGUF reference model. MicroK can load it from storage,
  parse metadata/tensors, and expose tokenizer/architecture info, but it does
  not run generative GGUF inference yet.

## GRUB Entry

Use the MKLM file to test in-kernel responses:

```cfg
menuentry "MicroK AI MKLM" {
    multiboot /boot/kernel.bin
    module /boot/model.mklm llm model
    boot
}
```

After boot:

```text
llm status
llm ask hola
llm ask estado
!kernel
```

## GGUF Smoke Test

Use this to test GGUF loading, metadata reporting, token tracing, and the
minimal first-token generation preview when available.

```bash
make qemu-gguf
```

Inside MicroK:

```text
loadmodel tin1.gguf
llm status
llm info
llm trace on
llm ask gguf
```

Expected:

- `loadmodel` succeeds.
- `llm status` reports either `PARSER-ONLY` or `GENERATIVE-PREVIEW`.
- `llm info` shows Llama-like metadata such as layers, embedding size, heads,
  context, vocab, and tensor count.
- `llm trace on` enables token trace logging for GGUF prompts and any generated
  preview token.
- if the model stays in `PARSER-ONLY`, `llm ask ...` reports that generation is
  not implemented for that backend state.
