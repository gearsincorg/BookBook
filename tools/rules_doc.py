"""Writes docs/conversation-rules.md from the persona prompt in main/brain.cpp.

The prompt (kSystemPrompt) is the single source of truth for how the librarian talks and behaves. Its rules are
numbered, and a line comment such as "// == Speaking ==" starts a new section of the document.

    python tools/rules_doc.py
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
src = (root / "main" / "brain.cpp").read_text(encoding="utf-8")

start = src.index("const char kSystemPrompt[] =")
block = src[start:src.index(";\n", start)]

lit = re.compile(r'"((?:[^"\\]|\\.)*)"')


def unescape(s):
    return s.replace('\\"', '"').replace("\\n", "\n")


intro = ""
sections = []  # [title, text]
for line in block.splitlines()[1:]:
    s = line.strip()
    m = re.match(r"// == (.+) ==", s)
    if m:
        sections.append([m.group(1), ""])
        continue
    text = unescape("".join(lit.findall(s)))
    if sections:
        sections[-1][1] += text
    else:
        intro += text

intro = intro.split("Rules:")[0]
out = [
    "# Conversation rules",
    "",
    "How the librarian (Marian Paroo) talks and behaves. **Generated from `main/brain.cpp` by "
    "`tools/rules_doc.py`: change the rules there, then run the script.** The rules are sent to Claude with every "
    "request, so a change needs a rebuild and reflash. The tool descriptions in the same file also steer "
    "behaviour.",
    "",
    "Background Claude is given before the rules: " + " ".join(intro.split()),
    "",
]
count = 0
for title, text in sections:
    out += ["## " + title, ""]
    for rule in re.split(r"\n(?=\d+\. )", text.strip()):
        out += [" ".join(rule.split()), ""]
        count += 1
(root / "docs" / "conversation-rules.md").write_text("\n".join(out), encoding="utf-8")
print(f"{count} rules in {len(sections)} sections")
