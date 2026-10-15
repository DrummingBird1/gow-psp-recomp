"""Cross-checks pspelf's relocator against Ghidra's loaded image (both at base 0).

Usage: python -I verify_reloc.py <EBOOT.BIN> <program.image0.bin> <program.image0.json>
"""
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from pspelf import PspModule  # noqa: E402


def main() -> int:
    elf, ghidra_img, ghidra_map = sys.argv[1:4]
    m = PspModule.parse(Path(elf).read_bytes())
    m.load(0)
    g = Path(ghidra_img).read_bytes()
    blocks = json.loads(Path(ghidra_map).read_text())["blocks"]
    ours = bytes(m.image)
    print(f"relocations: {m.reloc_stats}")
    print(f"image sizes: ours={len(ours)} ghidra={len(g)}")
    diffs = Counter()
    first = {}
    for b in blocks:
        s, n = b["start"], b["size"]
        for off in range(0, n, 4):
            a = s + off
            if ours[a:a + 4] != g[a:a + 4]:
                diffs[b["name"]] += 1
                first.setdefault(b["name"], a)
    total_words = sum(b["size"] // 4 for b in blocks)
    if not diffs:
        print(f"MATCH: all {total_words} words in {len(blocks)} Ghidra blocks are identical")
        return 0
    for name, n in diffs.most_common():
        a = first[name]
        print(f"DIFF {name}: {n} words, first at {a:#x}: ours={ours[a:a+4].hex()} ghidra={g[a:a+4].hex()}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
