"""Mutate generated images to exercise fragmentation and metadata rejection."""
import struct


def fragment_fat(image):
    """Swap two payload clusters and relink every FAT copy to preserve its data."""
    with image.open("r+b") as stream:
        base = 1024 * 1024
        stream.seek(base)
        boot = stream.read(512)
        sector = struct.unpack_from("<H", boot, 11)[0]
        cluster_size = sector * boot[13]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        roots = struct.unpack_from("<H", boot, 17)[0]
        fat_size = struct.unpack_from("<H", boot, 22)[0] or struct.unpack_from("<I", boot, 36)[0]
        total = struct.unpack_from("<H", boot, 19)[0] or struct.unpack_from("<I", boot, 32)[0]
        root_sectors = (roots * 32 + sector - 1) // sector
        data_sector = reserved + boot[16] * fat_size + root_sectors
        clusters = (total - data_sector) // boot[13]
        bits = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
        fat = base + reserved * sector
        data = base + data_sector * sector

        def read(offset, size):
            stream.seek(offset)
            return stream.read(size)

        def cluster_offset(cluster):
            return data + (cluster - 2) * cluster_size

        def next_cluster(cluster):
            offset = cluster * bits // 8
            n = int.from_bytes(read(fat + offset, 4 if bits == 32 else 2), "little")
            return ((n >> 4) if bits == 12 and cluster % 2 else n) & ((1 << bits) - 1)

        def find(entries, name):
            for off in range(0, len(entries), 32):
                e = entries[off:off + 32]
                if e[:11] == name:
                    return struct.unpack_from("<H", e, 26)[0] | (
                        struct.unpack_from("<H", e, 20)[0] << 16 if bits == 32 else 0)
            raise AssertionError(f"fixture directory entry missing: {name}")

        if bits == 32:
            root = read(cluster_offset(struct.unpack_from("<I", boot, 44)[0]), cluster_size)
        else:
            root = read(base + (reserved + boot[16] * fat_size) * sector, roots * 32)
        directory = find(root, b"BOOT       ")
        first = find(read(cluster_offset(directory), cluster_size), b"PAYLOAD BIN")
        chain = [first]
        for _ in range(4):
            chain.append(next_cluster(chain[-1]))
        a, b, c, d, e = chain
        old_b, old_d = read(cluster_offset(b), cluster_size), read(cluster_offset(d), cluster_size)
        stream.seek(cluster_offset(b)); stream.write(old_d)
        stream.seek(cluster_offset(d)); stream.write(old_b)
        for copy in range(boot[16]):
            for old, new in ((a, d), (d, c), (c, b), (b, e)):
                address = fat + copy * fat_size * sector + old * bits // 8
                length = 4 if bits == 32 else 2
                word = int.from_bytes(read(address, length), "little")
                shift = 4 if bits == 12 and old % 2 else 0
                mask = (1 << (28 if bits == 32 else bits)) - 1
                word = (word & ~(mask << shift)) | new << shift
                stream.seek(address); stream.write(word.to_bytes(length, "little"))
        print(f"Fragmented FAT{bits} payload allocation", flush=True)


def flip(image, offset, mask=1):
    with image.open("r+b") as stream:
        stream.seek(offset)
        old = stream.read(1)
        if len(old) != 1:
            raise AssertionError("fixture offset outside image")
        stream.seek(offset)
        stream.write(bytes([old[0] ^ mask]))
