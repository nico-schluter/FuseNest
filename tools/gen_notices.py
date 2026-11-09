#!/usr/bin/env python3
"""Builds packaging/resources/notices.html from the real build graph.

Not a hand-maintained list: cargo metadata is asked what is actually linked
for the target, and each crate's licence text is read out of the registry
source it was built from. A hand-written notice file is wrong the first time
a dependency moves, and nobody notices until someone asks.
"""
import hashlib
import html
import json
import pathlib
import subprocess
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGETS = ["aarch64-apple-darwin", "x86_64-apple-darwin", "x86_64-pc-windows-msvc"]

# Crates we ship as source in the tree rather than pulling from the registry.
VENDORED = {"sparrow": ROOT / "vendor" / "sparrow"}

# Canonical texts, for crates that declare a licence but ship no copy of it --
# which is most of them, since Cargo only requires the SPDX field. See
# packaging/licenses/SOURCES.md for where each came from.
CANONICAL = ROOT / "packaging" / "licenses"


def metadata(target):
    out = subprocess.run(
        ["cargo", "metadata", "--format-version", "1", "--filter-platform", target,
         "--manifest-path", str(ROOT / "nest_kernel" / "Cargo.toml")],
        capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def reachable(resolve):
    """Packages reachable from the root through NORMAL dependencies only.

    Not every node in the resolve graph: that includes dev- and
    build-dependencies, and for this graph most of them come from sparrow's
    own binaries -- clap, a TUI, a benchmark harness, a test-case macro. None
    of it is linked into the staticlib, and claiming to redistribute a crate
    we do not ship is as wrong in a notices file as omitting one we do.
    """
    nodes = {n["id"]: n for n in resolve["nodes"]}
    seen, stack = set(), [resolve["root"]]
    while stack:
        nid = stack.pop()
        if nid in seen:
            continue
        seen.add(nid)
        for dep in nodes[nid]["deps"]:
            kinds = dep.get("dep_kinds") or [{"kind": None}]
            # kind null is a normal dependency; "dev" and "build" are not
            # linked into what we ship.
            if any(k.get("kind") is None for k in kinds):
                stack.append(dep["pkg"])
    return seen


def linked_packages():
    """Every package actually linked, for every target we ship.

    The union, not one platform's: a Windows-only crate is still a crate whose
    licence has to appear in the Windows build's notices, and one notices file
    for both builds is less likely to go stale than two.
    """
    found = {}
    for target in TARGETS:
        try:
            m = metadata(target)
        except subprocess.CalledProcessError as e:
            print(f"warning: no metadata for {target}: {e.stderr.strip()}", file=sys.stderr)
            continue
        pkgs = {p["id"]: p for p in m["packages"]}
        for pid in reachable(m["resolve"]):
            p = pkgs[pid]
            if p["name"] == "nest_kernel":
                continue  # ours
            found[(p["name"], p["version"])] = p
    return [found[k] for k in sorted(found)]


def licence_texts(pkg):
    """The licence files shipped inside the crate, as (filename, text)."""
    src = VENDORED.get(pkg["name"])
    if src is None:
        manifest = pathlib.Path(pkg["manifest_path"])
        src = manifest.parent
    if not src.is_dir():
        return []
    out = []
    for path in sorted(src.iterdir()):
        name = path.name.upper()
        if not path.is_file():
            continue
        if name.startswith(("LICENSE", "LICENCE", "COPYING", "NOTICE", "UNLICENSE")):
            try:
                text = path.read_text(encoding="utf-8", errors="replace").strip()
            except OSError:
                continue
            if text:
                out.append((path.name, text))
    return out


def canonical_for(spdx):
    """The texts covering an SPDX expression, as (id, text).

    An OR expression is a choice the user makes, not one we make for them, so
    every branch is included. AND means all of them apply. Splitting on both
    words and stripping the parentheses is enough for the expressions actually
    in this graph; anything it cannot resolve is reported rather than guessed.
    """
    if not spdx:
        return [], []
    parts = (spdx.replace("(", " ").replace(")", " ")
                 .replace(" OR ", " ").replace(" AND ", " ").replace("/", " ").split())
    found, unknown = [], []
    for ident in dict.fromkeys(parts):
        path = CANONICAL / f"{ident}.txt"
        if path.is_file():
            found.append((ident, path.read_text(encoding="utf-8").strip()))
        else:
            unknown.append(ident)
    return found, unknown


def main():
    packages = linked_packages()

    # One copy of each distinct licence text, however many crates carry it:
    # a hundred crates share about six documents between them, and a notices
    # file nobody can scroll is a notices file nobody reads.
    bodies = {}
    users = defaultdict(list)
    unresolved = []
    for pkg in packages:
        texts = [t for _, t in licence_texts(pkg)]
        if not texts:
            # No file in the crate: fall back to the canonical text of what it
            # declares, and name the authors beside it rather than writing them
            # into a copyright line upstream never wrote.
            found, unknown = canonical_for(pkg.get("license"))
            texts = [t for _, t in found]
            if unknown:
                unresolved.append((pkg, unknown))
        for text in texts:
            key = hashlib.sha256(text.encode()).hexdigest()[:16]
            bodies[key] = text
            users[key].append(pkg)

    rows = []
    for pkg in packages:
        repo = pkg.get("repository") or ""
        rows.append(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                html.escape(pkg["name"]),
                html.escape(pkg["version"]),
                html.escape(pkg.get("license") or "see below"),
                '<a href="{0}">{0}</a>'.format(html.escape(repo)) if repo else "",
            ))

    sections = []
    for key, text in sorted(bodies.items(), key=lambda kv: -len(users[kv[0]])):
        crates = sorted({p["name"] for p in users[key]})
        holders = sorted({a for p in users[key] for a in (p.get("authors") or [])})
        head = "<h3>{}</h3>".format(html.escape(", ".join(crates)))
        if "<copyright holders>" in text and holders:
            head += '\n<p class="holders">Copyright holders, as the crates declare them: {}</p>'.format(
                html.escape("; ".join(holders)))
        sections.append("{}\n<pre>{}</pre>".format(head, html.escape(text)))

    page = PAGE.format(
        count=len(packages),
        rows="\n".join(rows),
        sections="\n".join(sections),
        missing=("<p class=\"note\">Could not resolve a licence text for: "
                 + html.escape(", ".join(f"{p['name']} ({'/'.join(u)})"
                                         for p, u in unresolved))
                 + ". The SPDX expression in the table above is the crate's own "
                   "declaration; the repository link is the source.</p>")
        if unresolved else "",
    )
    out = ROOT / "packaging" / "resources" / "notices.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8")
    print(f"wrote {out} -- {len(packages)} packages, {len(bodies)} distinct licence texts")
    if unresolved:
        print("UNRESOLVED: " + ", ".join(f"{p['name']} ({'/'.join(u)})"
                                         for p, u in unresolved))
        return 1
    return 0


PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>FuseNest &mdash; third-party notices</title>
<style>
  body {{ font: 13px/1.5 "Segoe UI", -apple-system, sans-serif; color: #222;
         margin: 1.5em; max-width: 46em; }}
  h1 {{ font-size: 1.4em; margin: 0 0 0.2em; }}
  h2 {{ font-size: 1.1em; margin: 1.6em 0 0.4em; }}
  h3 {{ font-size: 0.95em; margin: 1.6em 0 0.3em; font-weight: 600; }}
  p.lead {{ color: #555; margin-top: 0; }}
  a {{ color: #0479B3; }}
  table {{ border-collapse: collapse; margin: 0.6em 0; font-size: 0.95em; }}
  th, td {{ text-align: left; padding: 0.2em 1.2em 0.2em 0;
           border-bottom: 1px solid #e8e8e8; vertical-align: top; }}
  pre {{ background: #f7f7f7; padding: 0.8em; overflow-x: auto;
        white-space: pre-wrap; font-size: 0.9em; }}
  p.holders {{ color: #555; margin: 0.2em 0 0.4em; font-size: 0.95em; }}
  p.note {{ background: #f7f7f7; border-left: 3px solid #0479B3;
           padding: 0.6em 0.8em; margin: 0.8em 0; }}
</style>
</head>
<body>

<h1>Third-party notices</h1>
<p class="lead">FuseNest&#39;s solver is built on {count} open-source packages.
This page is generated from the dependency graph rather than maintained by
hand, so it cannot fall behind the build.</p>

<p class="note">The list is deliberately inclusive. It is every crate reachable
through normal dependencies on every platform FuseNest ships for, which is a
superset of what any one build links: a few are used only by sparrow&#39;s own
standalone command-line tools, and Cargo does not distinguish those from the
library&#39;s own. Naming a package we do not ship is harmless; omitting one we
do is not.</p>

<h2>Packages</h2>
<table>
<tr><th>Package</th><th>Version</th><th>Licence</th><th>Source</th></tr>
{rows}
</table>
{missing}

<h2>Licence texts</h2>
{sections}

</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
