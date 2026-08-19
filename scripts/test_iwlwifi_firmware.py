#!/usr/bin/env python3
import struct
from pathlib import Path

MAGIC = 0x0A4C5749
RUNTIME_TYPES = {19, 24}
INIT_TYPES = {20, 25}
SEPARATORS = {0xFFFFCCCC, 0xAAAABBBB}
HEADER_SIZE = 88


def parse(path: Path):
    data = path.read_bytes()
    assert len(data) >= HEADER_SIZE
    zero, magic = struct.unpack_from("<II", data)
    assert zero == 0 and magic == MAGIC

    offset = HEADER_SIZE
    tlvs = runtime = separators = runtime_bytes = 0
    init = init_separators = init_bytes = 0
    phy_config = None
    calibrations = {}
    paging_size = None
    command_versions = {}
    max_scan_channels = None
    runtime_layout = []
    while offset < len(data):
        assert len(data) - offset >= 8
        tlv_type, length = struct.unpack_from("<II", data, offset)
        offset += 8
        assert length <= len(data) - offset
        if tlv_type in RUNTIME_TYPES:
            assert length >= 4
            destination = struct.unpack_from("<I", data, offset)[0]
            runtime += 1
            separators += destination in SEPARATORS
            runtime_bytes += max(0, length - 4)
            runtime_layout.append((destination, length - 4))
        elif tlv_type in INIT_TYPES:
            assert length >= 4
            destination = struct.unpack_from("<I", data, offset)[0]
            init += 1
            init_separators += destination in SEPARATORS
            init_bytes += max(0, length - 4)
        elif tlv_type == 23:
            assert length == 4
            phy_config = struct.unpack_from("<I", data, offset)[0]
        elif tlv_type == 22:
            assert length == 12
            image, flow, event = struct.unpack_from("<III", data, offset)
            assert image in (0, 1)
            calibrations[image] = (flow, event)
        elif tlv_type == 32:
            assert length == 4
            paging_size = struct.unpack_from("<I", data, offset)[0]
        elif tlv_type == 48:
            assert length % 4 == 0
            for entry in range(offset, offset + length, 4):
                command, group, command_version, notification_version = \
                    struct.unpack_from("<BBBB", data, entry)
                command_versions[(group, command)] = \
                    (command_version, notification_version)
        elif tlv_type == 31:
            assert length == 4
            max_scan_channels = struct.unpack_from("<I", data, offset)[0]
        tlvs += 1
        offset += (length + 3) & ~3
        assert offset <= len(data)

    assert offset == len(data)
    assert runtime > 0 and separators > 0
    # This API-46 file has nine INIT sections and no explicit separator TLV;
    # its regular image carries the CPU and paging separators.
    assert init > 0
    assert phy_config is not None and (phy_config >> 16) & 0xF
    assert (phy_config >> 20) & 0xF
    assert calibrations.keys() == {0, 1}
    assert paging_size and paging_size % 4096 == 0
    assert command_versions[(1, 0x0C)][0] == 2   # SCAN_CFG_CMD
    assert command_versions[(1, 0x0D)][0] == 8   # SCAN_REQ_UMAC
    assert command_versions[(1, 0x18)][0] == 10  # ADD_STA
    assert command_versions[(1, 0x08)][0] == 2   # PHY_CONTEXT_CMD
    assert command_versions[(1, 0x1C)][0] == 6   # TX_CMD (9000, pre-gen2)
    assert command_versions[(1, 0x17)][0] == 1   # ADD_STA_KEY
    assert command_versions[(1, 0x28)][0] == 4   # MAC_CONTEXT_CMD
    assert command_versions[(1, 0x2B)][0] == 1   # BINDING_CONTEXT_CMD v1
    assert max_scan_channels and max_scan_channels <= 63
    paging_separator = next(i for i, (destination, _) in enumerate(runtime_layout)
                            if destination == 0xAAAABBBB)
    assert paging_separator + 2 < len(runtime_layout)
    assert runtime_layout[paging_separator + 1][1] <= 4096
    assert runtime_layout[paging_separator + 2][1] == paging_size
    return (len(data), tlvs, runtime, separators, runtime_bytes,
            init, init_separators, init_bytes, phy_config, calibrations,
            paging_size)


if __name__ == "__main__":
    fw = Path("firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode")
    (size, tlvs, runtime, separators, runtime_bytes,
     init, init_separators, init_bytes, phy_config, calibrations,
     paging_size) = parse(fw)
    print(f"iwlwifi firmware: OK size={size} tlvs={tlvs} runtime={runtime} "
          f"separators={separators} runtime_bytes={runtime_bytes} init={init} "
          f"init_separators={init_separators} init_bytes={init_bytes} "
          f"phy=0x{phy_config:08x} calib={len(calibrations)} "
          f"paging={paging_size}")
