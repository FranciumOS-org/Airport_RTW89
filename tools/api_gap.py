#!/usr/bin/env python3
"""Linux API gap report for porting rtw89 (RTL8852BE PCIe) onto the Feixiao compat layer.

Collects every external identifier the rtw89 8852BE subset references (functions,
macros, struct types) and reports which ones nothing in the tree defines: neither
the compat layer (src/) nor the vendored upstream headers (third_party/linux-include).

This is a regex estimate for sizing work after an upstream bump. The compiler
(`make -k compile && make errors`) and `make link` are the ground truth.

Usage: python tools/api_gap.py [--rtw89 DIR] [--compat DIR ...] [--out docs/api-gap.md]
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RTW89 = ROOT / "third_party/rtw89"
DEFAULT_COMPAT = [ROOT / "src", ROOT / "third_party/linux-include"]

# Files that make up rtw89_core + rtw89_pci + the 8852B/8852BE chip modules.
SUBSET = [
    "core", "mac80211", "mac", "mac_be", "phy", "phy_be", "fw", "cam", "efuse",
    "efuse_be", "regd", "sar", "coex", "ps", "chan", "ser", "acpi", "util", "wow",
    "pci", "pci_be", "rtw8852b_common", "rtw8852b", "rtw8852b_table",
    "rtw8852b_rfk", "rtw8852b_rfk_table", "rtw8852be",
]

C_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "case", "do", "else",
    "goto", "typeof", "__typeof__", "defined", "offsetof", "static", "const",
    "unsigned", "signed", "int", "char", "void", "long", "short", "bool",
    "struct", "union", "enum", "inline", "volatile", "__attribute__",
    "_Static_assert", "static_assert", "alignof", "__alignof__",
}

CALL_RE = re.compile(r"(?<![\w.>])([A-Za-z_][A-Za-z0-9_]*)\s*\(")  # skip ops->fn( and s.fn(
# DECLARE_EWMA(name, ...) generates ewma_<name>_{init,add,read}; those come from the macro.
EWMA_RE = re.compile(r"DECLARE_EWMA\(\s*(\w+)")
STRUCT_RE = re.compile(r"\b(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)")
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"')


def strip(src: str) -> str:
    return STRING_RE.sub('""', COMMENT_RE.sub(" ", src))


def definitions(text: str) -> set[str]:
    """Names that a source/header defines: functions, macros, types, enumerators."""
    names = set()
    names.update(re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)", text, re.M))
    # function definitions/prototypes: `type name(` at line start (possibly multi-token type)
    names.update(re.findall(r"^[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*[{;]", text, re.M))
    names.update(m[1] for m in STRUCT_RE.findall(text))
    names.update(re.findall(r"typedef\s[^;]*?\b([A-Za-z_]\w*)\s*;", text))
    # enumerators
    for body in re.findall(r"\benum\b[^{;]*\{([^}]*)\}", text):
        names.update(re.findall(r"^\s*([A-Za-z_]\w*)", body, re.M))
    # static inline helpers split across lines
    names.update(re.findall(r"\b([A-Za-z_]\w*)\s*\([^)]*\)\s*\n?\s*\{", text))
    return names


def classify(name: str) -> str:
    prefixes = [
        ("mac80211", ("ieee80211_", "IEEE80211_", "wiphy_", "WIPHY_")),
        ("cfg80211", ("cfg80211_", "nl80211_", "NL80211_", "regulatory_", "reg_")),
        ("skb/net", ("skb_", "__skb", "dev_kfree_skb", "netdev_", "napi_", "ether_", "eth_", "netif_", "NET_")),
        ("pci/dma", ("pci_", "PCI_", "dma_", "DMA_", "pcie_", "pcim_")),
        ("sync/work", ("mutex_", "spin_", "rcu_", "RCU_", "lockdep_", "queue_", "cancel_", "flush_",
                       "INIT_", "schedule_", "wait_", "complete", "reinit_", "init_completion",
                       "local_bh", "synchronize_", "atomic_", "timer_", "del_timer", "mod_timer",
                       "hrtimer", "tasklet", "irq", "wake_up", "alloc_workqueue", "destroy_workqueue",
                       "list_", "hlist_", "READ_ONCE", "WRITE_ONCE", "smp_", "guard", "scoped_")),
        ("memory", ("kmalloc", "kzalloc", "kcalloc", "kfree", "kmemdup", "vmalloc", "vzalloc", "vfree",
                    "devm_", "krealloc", "kvfree", "kvzalloc", "kvmalloc", "__free", "free_")),
        ("bits/util", ("BIT", "GENMASK", "FIELD_", "u32_", "le32_", "le16_", "le64_", "cpu_to_", "be32_",
                       "be16_", "u8_", "u16_", "u64_", "ffs", "fls", "__ffs", "hweight", "bitmap_",
                       "set_bit", "clear_bit", "test_", "__set_bit", "__clear_bit", "for_each_",
                       "ARRAY_SIZE", "min", "max", "clamp", "abs", "DIV_ROUND", "roundup", "rounddown",
                       "ALIGN", "BUILD_BUG", "container_of", "IS_ERR", "PTR_ERR", "ERR_PTR", "WARN",
                       "BUG", "likely", "unlikely", "memcpy", "memset", "memcmp", "str", "snprintf",
                       "scnprintf", "sprintf", "sscanf", "kstrto", "ether_addr", "is_zero_ether",
                       "is_broadcast", "is_multicast", "get_unaligned", "put_unaligned", "swap",
                       "struct_size", "flex_array", "array_size", "size_", "check_", "sign_extend",
                       "DECLARE_", "DEFINE_", "__packed", "__le", "le32p", "u32p")),
        ("time", ("jiffies", "msecs_to", "usecs_to", "msleep", "usleep", "udelay", "mdelay", "fsleep",
                  "ktime", "time_", "read_poll", "readx_poll", "HZ", "ns_to", "ms_to")),
        ("firmware", ("request_firmware", "release_firmware", "firmware_")),
        ("log/debug", ("dev_", "pr_", "print_hex", "seq_", "debugfs_", "trace_", "rtw89_debug", "WARN_ON")),
        ("acpi/platform", ("acpi_", "ACPI_", "dmi_", "DMI_", "efi_", "device_", "of_")),
        ("pm/wow", ("pm_", "PM_", "SIMPLE_DEV_PM", "SET_", "device_wakeup", "wowlan")),
    ]
    for cat, pre in prefixes:
        if name.startswith(pre):
            return cat
    return "other"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rtw89", type=Path, default=DEFAULT_RTW89)
    ap.add_argument("--compat", type=Path, nargs="+", default=DEFAULT_COMPAT)
    ap.add_argument("--out", type=Path, default=ROOT / "docs/api-gap.md")
    args = ap.parse_args()

    driver_text = {}
    for stem in SUBSET:
        src = args.rtw89 / f"{stem}.c"
        if not src.exists():  # not vendored (wow.c until the sleep/wake milestone)
            continue
        driver_text[f"{stem}.c"] = strip(src.read_text(encoding="utf-8", errors="replace"))
    header_text = {p.name: strip(p.read_text(encoding="utf-8", errors="replace")) for p in args.rtw89.glob("*.h")}

    # Everything rtw89 itself defines is internal and doesn't need a shim.
    internal = set()
    for t in list(driver_text.values()) + list(header_text.values()):
        internal |= definitions(t)
        for ew in EWMA_RE.findall(t):
            internal |= {f"ewma_{ew}_init", f"ewma_{ew}_add", f"ewma_{ew}_read"}

    compat_defs = set()
    compat_files = [p for d in args.compat for p in d.rglob("*")
                    if p.suffix in {".h", ".hpp", ".c", ".cpp"}]
    for p in compat_files:
        compat_defs |= definitions(strip(p.read_text(encoding="utf-8", errors="replace")))

    used = defaultdict(lambda: {"count": 0, "files": set(), "kind": "call"})
    for fname, text in list(driver_text.items()) + [(n, t) for n, t in header_text.items()
                                                      if n.split(".")[0] in SUBSET or n in {"txrx.h", "reg.h"}]:
        for name in CALL_RE.findall(text):
            if name in C_KEYWORDS or name in internal or name.startswith("rtw89_") or name.startswith("RTW89_"):
                continue
            used[name]["count"] += 1
            used[name]["files"].add(fname)
        for _, name in STRUCT_RE.findall(text):
            if name in internal or name.startswith("rtw89"):
                continue
            used[name]["count"] += 1
            used[name]["files"].add(fname)
            used[name]["kind"] = "type"

    missing = {n: v for n, v in used.items() if n not in compat_defs}
    present = {n: v for n, v in used.items() if n in compat_defs}

    by_cat = defaultdict(list)
    for n, v in missing.items():
        by_cat[classify(n)].append((v["count"], n, v))

    lines = [
        "# rtw89 (RTL8852BE) → compat layer: API gap",
        "",
        "Generated by `tools/api_gap.py`. Heuristic (regex, not a compiler) — treat as a work list, "
        "not ground truth. Names defined inside rtw89 itself are excluded.",
        "",
        f"- External identifiers referenced by the 8852BE subset: **{len(used)}**",
        f"- Defined by the compat layer or the vendored upstream headers: **{len(present)}**",
        f"- Missing (need a shim or reimplementation): **{len(missing)}**",
        "",
        "| Category | Missing |",
        "|---|---|",
    ]
    for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
        lines.append(f"| {cat} | {len(by_cat[cat])} |")
    for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
        lines += ["", f"## {cat}", "", "| Identifier | Kind | Uses | Files |", "|---|---|---|---|"]
        for count, n, v in sorted(by_cat[cat], key=lambda x: (-x[0], x[1])):
            files = ", ".join(sorted(v["files"])[:4]) + (" …" if len(v["files"]) > 4 else "")
            lines.append(f"| `{n}` | {v['kind']} | {count} | {files} |")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"used={len(used)} present={len(present)} missing={len(missing)} -> {args.out}")
    for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
        print(f"  {cat:14s} {len(by_cat[cat])}")


if __name__ == "__main__":
    main()
