#!/usr/bin/env python3

import argparse
import os
import sys

import pycriu.images


PE_PRESENT = 4
PE_PAYLOAD_ALIGNED = 8


def check_raw_entry(directory, pid, expected_layout):
    path = os.path.join(directory, "pagemap-%s.img" % pid)

    with open(path, "rb") as image:
        entries = pycriu.images.load(image)["entries"][1:]

    page_size = os.sysconf("SC_PAGE_SIZE")
    payload_offset = 0
    found = False
    found_lz4 = False
    found_padded_lz4 = False
    for entry in entries:
        flags = int(entry.get("flags", 0))
        if not flags & PE_PRESENT:
            continue
        if flags & PE_PAYLOAD_ALIGNED:
            payload_offset = (payload_offset + page_size - 1) & -page_size

        is_raw_target = (
            int(entry.get("nr_pages", 0)) == 56
            and "blocks" not in entry
        )
        if is_raw_target:
            if not flags & PE_PAYLOAD_ALIGNED:
                print("FAIL: all-raw entry lacks PE_PAYLOAD_ALIGNED in %s" % path)
                return 1
            if payload_offset % page_size:
                print(
                    "FAIL: all-raw entry starts at unaligned offset %d"
                    % payload_offset
                )
                return 1
            found = True

        blocks = entry.get("blocks")
        if blocks and blocks.get("block_sizes"):
            padded = bool(blocks.get("payload_padded", False))
            if expected_layout == "padded" and not padded:
                print("FAIL: compressed entry is not padded in %s" % path)
                return 1
            if expected_layout == "packed" and padded:
                print(
                    "FAIL: packed capture unexpectedly uses padding in %s" % path
                )
                return 1
            block_pages = int(blocks.get("pages_per_block", 1))
            remaining = int(entry.get("nr_pages", 0))
            expected_total = 0
            for size in blocks["block_sizes"]:
                size = int(size)
                expected_total += (
                    (size + page_size - 1) & -page_size
                    if padded and size
                    else size
                )
                pages = min(block_pages, remaining)
                if 0 < size < pages * page_size:
                    found_lz4 = True
                    found_padded_lz4 |= padded
                remaining -= pages
            total = int(blocks.get("total_payload_size", expected_total))
            if total != expected_total:
                print(
                    "FAIL: payload total %d does not match physical block sum %d in %s"
                    % (total, expected_total, path)
                )
                return 1
            payload_offset += total
        else:
            payload_offset += int(entry.get("nr_pages", 0)) * page_size

    if not found:
        print(
            "FAIL: no aligned 56-page all-raw entry without compression metadata "
            "in %s" % path
        )
        return 1
    if not found_lz4:
        print("FAIL: compression test produced no LZ4-compressed block in %s" % path)
        return 1
    if expected_layout == "padded" and not found_padded_lz4:
        print("FAIL: padded capture produced no padded LZ4 block in %s" % path)
        return 1

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Validate raw-fallback pagemap metadata"
    )
    parser.add_argument("directory")
    parser.add_argument("pid")
    parser.add_argument("layout", choices=("packed", "padded"))
    args = parser.parse_args()
    return check_raw_entry(args.directory, args.pid, args.layout)


if __name__ == "__main__":
    sys.exit(main())
