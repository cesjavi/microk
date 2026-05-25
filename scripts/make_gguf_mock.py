import struct
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: make_gguf_mock.py <output_file>")
        return

    output_file = sys.argv[1]

    # GGUF Header:
    # uint32 magic = 0x46554747
    # uint32 version = 3
    # uint64 tensor_count = 10
    # uint64 metadata_count = 5

    magic = 0x46554747
    version = 3
    tensor_count = 10
    metadata_count = 5

    with open(output_file, 'wb') as f:
        f.write(struct.pack('<I I Q Q', magic, version, tensor_count, metadata_count))
        # Add some padding to make it > 0 size
        f.write(b'\x00' * 100)

    print(f"Created mock GGUF model at {output_file}")

if __name__ == "__main__":
    main()
