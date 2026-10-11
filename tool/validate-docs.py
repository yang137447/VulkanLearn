"""Validate the documents/ layering contract.

The point of this script is that documentation rot becomes a gate failure instead
of silent drift. It checks four things the layering promises:

1. every document referenced from documents/routing.md exists;
2. every source glob in routing.md's first column matches at least one real file;
3. every gate rule named in routing.md exists verbatim as a `-RuleName` in
   tool/ue-lite-boundary-audit.ps1 -- rules are never paraphrased in docs;
4. documents/contracts/** contains no future-tense assertion, because a contract
   that talks about future behaviour is how roadmaps got mistaken for reality here.

It also reports, without failing, which contract documents no source path routes
to, so an unrouted contract is visible rather than silently orphaned.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROUTING = "documents/routing.md"
CONTRACTS = "documents/contracts"
AUDIT_SCRIPT = "tool/ue-lite-boundary-audit.ps1"

RULE_NAME = re.compile(r'-RuleName\s+"([^"]+)"')
# A routing table cell holding a backticked document or diagram-IR path.
DOC_REF = re.compile(r"`((?:contracts|rationale)/[^`]+\.(?:md|html|json))`")
# A routing table cell holding a source glob or path.
PATH_GLOB = re.compile(r"`((?:source|shader|schema|tool|config|extern|ui|documents)/[^`]+)`")
# Table rows only: skip prose and headings.
TABLE_ROW = re.compile(r"^\|(.+)\|\s*$")

# Vocabulary that marks roadmap-style content. Deliberately narrow: a contract may
# legitimately state "not yet implemented" as a current fact, and may cite a planning
# document by name -- both are how this repository tells the truth about gaps. The
# failure this guards against is a contract *asserting future behaviour of its own*.
FUTURE_ASSERTION = re.compile(
    r"(将(会)?(实现|支持|改为|引入|迁移)|will be implemented|to be implemented|"
    r"计划(实现|支持|引入|迁移|改为)|下一步(实现|计划)|future implementation)",
    re.IGNORECASE)
# "X remains future work" / "X is out of scope" states a present boundary, not a
# promise about the future, so it is allowed in a contract.

# A contract that defers to a planning document for its own subject is not a contract.
PLAN_DEFERRAL = re.compile(r"`?(?:\.\./)*(?:documents/)?plan/[\w./-]+`?")
STATUS_LINE = re.compile(r"^\s*>?\s*状态[:：]")

# Documents that are rationale by construction and must not sit in contracts/.
NOT_CONTRACT = {"shading-model-case-records.md"}


def read(path):
    return path.read_text(encoding="utf-8-sig")


def split_rule_cell(cell):
    """Gate-rule cells separate rules with the ideographic comma. Fall back to a
    comma only when no ideographic comma is present, so a rule name containing a
    comma is never split."""
    text = cell.strip()
    if not text or text in ("—", "-"):
        return []
    separator = "、" if "、" in text else "," if "," in text else None
    if separator is None:
        return [text]
    return [part.strip() for part in text.split(separator) if part.strip()]


def check_routing(root, failures, notes):
    routing_path = root / ROUTING
    if not routing_path.is_file():
        failures.append("%s: missing" % ROUTING)
        return set()

    audit_path = root / AUDIT_SCRIPT
    if not audit_path.is_file():
        failures.append("%s: missing; cannot verify gate rule names" % AUDIT_SCRIPT)
        return set()
    known_rules = set(RULE_NAME.findall(read(audit_path)))
    if not known_rules:
        failures.append("%s: no -RuleName found" % AUDIT_SCRIPT)
        return set()

    routed = set()
    rows = 0
    # The first table after a "## " heading is the path-keyed one; later tables are
    # keyed by change type or topic, so demanding a source path in their first cell
    # would be a false failure -- but every table still registers the contracts it
    # names, so no contract can be orphaned.
    in_path_table = False
    header_seen = False
    for number, line in enumerate(read(routing_path).splitlines(), 1):
        if line.startswith("## "):
            in_path_table = "精确规则" in line
            header_seen = False
            continue
        match = TABLE_ROW.match(line)
        if match is None:
            continue
        cells = [cell.strip() for cell in match.group(1).split("|")]
        if len(cells) < 2:
            continue
        # A separator row marks the end of any header; a header row itself carries no
        # contract and must not be parsed as data.
        if all(set(cell) <= set("-: ") for cell in cells if cell):
            header_seen = True
            continue
        if not header_seen:
            continue
        rows += 1
        location = "%s:%d" % (ROUTING, number)

        # Register every document the row names, wherever the row puts it: the
        # path table puts it in the second cell, the topic table in the first.
        for cell in cells:
            for document in DOC_REF.findall(cell):
                target = root / "documents" / document
                if not target.is_file():
                    failures.append("%s: referenced document does not exist: %s"
                                    % (location, document))
                else:
                    routed.add(document)

        if not in_path_table:
            continue

        glob_count = 0
        for pattern in PATH_GLOB.findall(cells[0]):
            glob_count += 1
            if not list(root.glob(pattern)):
                failures.append("%s: source path matches nothing: %s" % (location, pattern))
        if glob_count == 0:
            failures.append("%s: routing row names no source path" % location)

        if len(cells) >= 3:
            for rule in split_rule_cell(cells[2]):
                if rule not in known_rules:
                    failures.append(
                        "%s: gate rule not found verbatim in %s: %r" % (location, AUDIT_SCRIPT, rule))

    notes.append("routing table rows checked: %d" % rows)
    notes.append("gate rules available in audit script: %d" % len(known_rules))
    return routed


def check_diagrams(root, routed, failures, notes):
    """Architecture diagrams are contracts too: their IR is the topology source.

    The check that matters is whether the *evidence still holds*: a diagram pins a
    revision and cites file/line ranges, so it has drifted when a cited path no
    longer exists or its line range no longer fits the file. Comparing the pinned
    SHA against HEAD would be worse than useless -- every commit to unrelated files
    (including documentation) would fail the gate and train people to ignore it.
    Use --update-revisions to advance the pins once the references still resolve.
    """
    diagrams = root / CONTRACTS / "diagrams"
    if not diagrams.is_dir():
        return
    import json

    count = 0
    for path in sorted(diagrams.glob("*.json")):
        if path.name.endswith((".finalize.json", ".delivery.json", ".finalize-summary.json",
                               ".browser-check.json")):
            continue
        count += 1
        relative = path.relative_to(root).as_posix()
        registered = "contracts/diagrams/" + path.name
        if registered not in routed:
            failures.append("orphan diagram, routed from no source path or topic: %s" % relative)
        try:
            data = json.loads(read(path))
        except (ValueError, OSError) as error:
            failures.append("%s: not valid JSON: %s" % (relative, error))
            continue
        revision = (data.get("meta", {}).get("repository", {}) or {}).get("revision", "")
        if not revision:
            failures.append("%s: diagram pins no meta.repository.revision" % relative)

        references = 0
        for node in data.get("components", []) + data.get("nodes", []) + \
                data.get("participants", []) + data.get("states", []):
            for source in node.get("sources", []) or []:
                references += 1
                cited = source.get("path", "")
                target = root / cited
                if not cited or not target.is_file():
                    failures.append("%s: node %r cites a missing path: %s"
                                    % (relative, node.get("id"), cited or "(empty)"))
                    continue
                if "line" not in source:
                    continue
                line_count = len(read(target).splitlines())
                end = source.get("end_line", source["line"])
                if not (1 <= source["line"] <= line_count) or not (1 <= end <= line_count):
                    failures.append(
                        "%s: node %r cites %s:%s-%s but the file has %d lines"
                        % (relative, node.get("id"), cited, source["line"], end, line_count))
        notes.append("%s: %d source reference(s) resolve" % (relative, references))


def update_revisions(root):
    """Advance each diagram's pinned revision to HEAD, for use after the cited
    references have been re-verified. Writes only the revision field."""
    import json
    import subprocess as sp

    head = sp.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True)
    if head.returncode != 0:
        print("cannot read HEAD")
        return 1
    revision = head.stdout.strip()
    diagrams = root / CONTRACTS / "diagrams"
    for path in sorted(diagrams.glob("*.json")):
        if path.name.endswith((".finalize.json", ".delivery.json", ".finalize-summary.json",
                               ".browser-check.json")):
            continue
        data = json.loads(read(path))
        repository = data.get("meta", {}).get("repository")
        if not repository:
            continue
        if repository.get("revision") == revision:
            print("%s: already at %s" % (path.name, revision[:12]))
            continue
        repository["revision"] = revision
        with open(path, "w", encoding="utf-8", newline="") as handle:
            json.dump(data, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
        print("%s: revision -> %s" % (path.name, revision[:12]))
    print("\nregenerate the HTML with archify finalize after this")
    return 0


def check_generated_index(root, routed, failures, notes):
    """The rule index is generated from the audit script, so it must not be stale.

    Keeping a second, hand-maintained copy of the 64 rule names was the original
    drift source; generating it removes the copy, and this check removes the chance
    of the generated copy going stale unnoticed.
    """
    index = root / CONTRACTS / "boundary-rules.generated.md"
    registered = "contracts/boundary-rules.generated.md"
    if registered not in routed:
        failures.append("generated rule index is routed from nowhere: %s" % registered)
    completed = subprocess.run(
        [sys.executable, str(root / "tool" / "generate_boundary_rule_index.py"), str(root), "--check"],
        capture_output=True, text=True)
    if completed.returncode != 0:
        message = (completed.stdout + completed.stderr).strip().splitlines()
        failures.append("generated rule index check failed: %s"
                        % (message[-1] if message else "unknown error"))
    else:
        notes.append(completed.stdout.strip() or "rule index OK")


def check_contracts(root, routed, failures, notes):
    contracts = root / CONTRACTS
    if not contracts.is_dir():
        failures.append("%s: missing" % CONTRACTS)
        return

    # A generated diagram HTML is owned by its IR, not by the contract rules: it has no
    # status line and must not be hand-edited. Its IR in diagrams/ is what gets routed
    # and checked by check_diagrams.
    generated_diagrams = {"contracts/" + path.name for path in (contracts / "diagrams").glob("*.html")}
    present = {"contracts/" + path.name for path in contracts.rglob("*")
               if path.suffix in (".md", ".html")} - generated_diagrams
    # An orphaned contract is exactly what this layering exists to surface, so it is a
    # failure rather than a note: the routing table must account for every contract.
    for document in sorted(present):
        if document not in routed:
            failures.append("orphan contract, routed from no source path or topic: %s" % document)

    check_diagrams(root, routed, failures, notes)
    check_generated_index(root, routed, failures, notes)

    for path in sorted(contracts.rglob("*.md")):
        relative = path.relative_to(root).as_posix()
        if path.name in NOT_CONTRACT:
            failures.append("%s: rationale document inside contracts/ (move to rationale/)"
                            % relative)
            continue
        lines = read(path).splitlines()
        if not any(STATUS_LINE.match(line) for line in lines[:40]):
            failures.append("%s: contract has no status line in its first 40 lines "
                            "(add '> 状态：...')" % relative)
        in_fence = False
        for number, line in enumerate(lines, 1):
            if line.lstrip().startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            match = FUTURE_ASSERTION.search(line)
            # A contract may need to quote the banned phrasing in order to define the
            # rule. That requires an explicit, reviewable marker on the line itself
            # rather than a blanket exemption, so a real assertion cannot hide.
            if match and "validate-docs: allow-future-assertion" not in line:
                failures.append(
                    "%s:%d: contract asserts future behaviour (%r): %s"
                    % (relative, number, match.group(), line.strip()[:90]))


def check_links(root, failures, notes):
    link = re.compile(r"!?\[[^\]\n]*\]\((<[^>\n]+>|[^)\n]+)\)")
    fence = re.compile(r"^\s*(```|~~~)")
    inline_code = re.compile(r"(`+)(.*?)\1")
    checked = 0
    outside = []
    for path in sorted((root / "documents").rglob("*.md")):
        relative = path.relative_to(root).as_posix()
        in_fence = False
        for number, line in enumerate(read(path).splitlines(), 1):
            if fence.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for match in link.finditer(inline_code.sub("", line)):
                destination = match.group(1).strip()
                if destination.startswith("<"):
                    destination = destination[1:-1]
                else:
                    destination = destination.split(" ", 1)[0]
                if destination.startswith(("http://", "https://", "#", "mailto:")):
                    continue
                link_path = destination.split("#", 1)[0]
                if not link_path:
                    continue
                # Sibling-checkout references (the personal knowledge base) resolve in
                # the primary checkout but not in a git worktree, so a missing target
                # outside the repository is reported as a note, not a failure.
                resolved = (path.parent / link_path).resolve()
                checked += 1
                if resolved.exists():
                    continue
                try:
                    resolved.relative_to(root)
                except ValueError:
                    outside.append("%s:%d: %s" % (relative, number, destination))
                    continue
                failures.append("%s:%d: local link target missing: %s"
                                % (relative, number, destination))
    notes.append("relative links checked: %d" % checked)
    if outside:
        # Usually hundreds of knowledge-base citations; a count plus the distinct
        # targets is actionable, one note per line is not.
        targets = sorted({" ".join(entry.split(": ", 1)[1].split("/")[:2]) for entry in outside})
        notes.append("links leaving the repository (verify in the primary checkout): %d across %s"
                     % (len(outside), ", ".join(targets)))
        for entry in outside[:5]:
            notes.append("  e.g. " + entry)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", nargs="?", default=".", type=Path)
    parser.add_argument("--quiet-notes", action="store_true")
    parser.add_argument("--update-revisions", action="store_true",
                        help="advance each diagram's pinned revision to HEAD, after the "
                             "cited references have been re-verified")
    args = parser.parse_args()
    root = args.repo.resolve()

    if args.update_revisions:
        return update_revisions(root)

    failures = []
    notes = []
    routed = check_routing(root, failures, notes)
    check_contracts(root, routed, failures, notes)
    check_links(root, failures, notes)

    if not args.quiet_notes:
        for note in notes:
            print("note: %s" % note)
    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        print("\n%d problem(s)" % len(failures), file=sys.stderr)
        return 1
    print("documents layering OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
