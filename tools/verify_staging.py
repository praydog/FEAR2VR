"""Verify (or regenerate) staging/SHA256SUMS.

    python tools/verify_staging.py          check every file against the manifest
    python tools/verify_staging.py --write  regenerate the manifest from what is there

Exists because `sha256sum -c` is unreliable on the volume this repo lives on: it returns FAILED for
files whose hash it computes correctly when asked directly, and does so in less time than reading
them would take -- i.e. its check path is not reading the file at all. Hashing here instead removes
that whole class of false alarm from CI.

Exit status is 0 when everything matches, 1 otherwise, so it drops straight into a workflow step.
"""

import hashlib
import pathlib
import sys

STAGING = pathlib.Path(__file__).resolve().parent.parent / "staging"
MANIFEST = STAGING / "SHA256SUMS"


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def payload_files() -> list[pathlib.Path]:
    # Everything staged EXCEPT the manifest and the prose describing it: those are ours, the rest is
    # what actually gets placed next to the game.
    skip = {MANIFEST.name, "README.md"}
    return sorted(p for p in STAGING.iterdir() if p.is_file() and p.name not in skip)


def write() -> int:
    lines = [f"{digest(p)}  {p.name}\n" for p in payload_files()]
    MANIFEST.write_text("".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {MANIFEST.relative_to(STAGING.parent)} ({len(lines)} entries)")
    return 0


def check() -> int:
    if not MANIFEST.exists():
        print(f"missing {MANIFEST} -- run with --write")
        return 1

    expected: dict[str, str] = {}
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        want, name = line.split(None, 1)
        expected[name.strip().lstrip("*")] = want

    bad = 0
    seen = set()
    for p in payload_files():
        seen.add(p.name)
        got = digest(p)
        want = expected.get(p.name)
        if want is None:
            print(f"UNLISTED  {p.name} ({got}) -- not in the manifest")
            bad += 1
        elif got != want:
            print(f"MISMATCH  {p.name}\n          expected {want}\n          got      {got}")
            bad += 1
        else:
            print(f"ok        {p.name}  {got[:16]}...  {p.stat().st_size} bytes")

    for name in expected.keys() - seen:
        print(f"MISSING   {name} -- listed in the manifest, absent from staging/")
        bad += 1

    print("staging verified" if bad == 0 else f"{bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(write() if "--write" in sys.argv[1:] else check())
