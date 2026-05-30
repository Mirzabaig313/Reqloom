#!/usr/bin/env bash
# Wrap every unquoted `description:` value in YAML frontmatter with
# double quotes so colons in the value don't trigger "bad indentation
# of a mapping entry".
#
# Idempotent. Run from any directory.
set -euo pipefail

cd "$(dirname "$0")/.."

python3 - <<'PYEOF'
from pathlib import Path
import re

DOCS = Path("src/content/docs")
PATTERN = re.compile(r'^description:\s+(.*)$')

count = 0
for path in DOCS.rglob("*"):
    if path.suffix not in (".md", ".mdx"):
        continue
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=False)

    inside_fm = False
    fm_seen = 0
    new_lines = []
    changed = False
    for line in lines:
        if line == "---":
            fm_seen += 1
            inside_fm = (fm_seen == 1)
            new_lines.append(line)
            continue

        if inside_fm:
            m = PATTERN.match(line)
            if m:
                value = m.group(1)
                # Skip if already quoted with " or '
                if not (value.startswith('"') or value.startswith("'")):
                    escaped = value.replace('\\', '\\\\').replace('"', '\\"')
                    new_lines.append(f'description: "{escaped}"')
                    changed = True
                    continue
        new_lines.append(line)

    if changed:
        path.write_text("\n".join(new_lines) + ("\n" if text.endswith("\n") else ""), encoding="utf-8")
        count += 1
        print(f"  fixed: {path.relative_to(DOCS)}")

print(f"\nTotal files fixed: {count}")
PYEOF
