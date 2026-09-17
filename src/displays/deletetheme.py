#!/usr/bin/env python3
"""
deletetheme.py — List or delete themes from themes.h.

USAGE:
    py deletetheme.py              # List themes with indices, then pick one to delete
    py deletetheme.py <index>      # Delete theme at index (0-based)
    py deletetheme.py -h | --help  # Show this help

Deleting ALWAYS asks for confirmation — including when the index is passed on
the command line.  There is no bypass flag.

EXAMPLES:
    py deletetheme.py
    py deletetheme.py 3
"""

import re, sys, os

# Force UTF-8 output on Windows
if sys.platform == 'win32':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'themes.h')


def _parse_names(content):
    """Return list of theme names from _themeNames block."""
    m = re.search(r'const char _themeNames\[\]\[\d+\] PROGMEM = \{', content)
    if not m:
        print("ERROR: Could not find _themeNames.")
        sys.exit(1)
    brace_end = content.find('\n};', m.end())
    if brace_end == -1:
        print("ERROR: Could not find closing } of _themeNames.")
        sys.exit(1)
    block = content[m.end():brace_end]
    names = [n.strip().strip('"').strip(',').strip('"') for n in block.strip().split('\n') if n.strip()]
    return names, m.start(), brace_end


def _parse_entries(content):
    """Return list of (start_pos, end_pos) for each theme entry in _themes[]."""
    m = re.search(r'const ThemeData _themes\[\]\s*PROGMEM\s*=\s*\{', content)
    if not m:
        print("ERROR: Could not find _themes[].")
        sys.exit(1)
    entries = []
    # Only scan entries after the _themes[] opening brace
    search_start = m.end()
    while True:
        em = re.search(r'(?m)^\s+\{\s+//', content[search_start:])
        if not em:
            break
        em_start = search_start + em.start()
        # Find the { in the match and start counting after it
        brace_pos = em_start + em.group().index('{')
        depth, i = 1, brace_pos + 1
        while i < len(content) and depth > 0:
            ch = content[i]
            if ch == '{': depth += 1
            elif ch == '}': depth -= 1
            i += 1
        if depth != 0:
            search_start = em_start + 1
            continue
        # Find the comma after the closing }
        comma = content.find(',', i)
        if comma == -1 or comma > i + 5:
            search_start = em_start + 1
            continue
        entries.append((em_start, comma + 1))
        search_start = comma + 1
    return entries, m.start()


def _ask_index(prompt, count, what):
    """Prompt for a 0-based index into 'count' entries.  Blank/EOF/^C cancels (None)."""
    while True:
        try:
            resp = input(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return None
        if resp == '':
            return None
        if resp.isdigit() and 0 <= int(resp) < count:
            return int(resp)
        print(f"  Please enter 0-{count - 1} for the {what}, or blank to cancel.")


def interactive_delete():
    """No-argument flow: list the themes, pick one, then delete it."""
    names = list_themes()
    if not names:
        return
    idx = _ask_index(f"\nDelete which theme? [0-{len(names) - 1}] (blank to cancel): ",
                     len(names), "theme")
    if idx is None:
        print("Cancelled. Nothing was changed.")
        return
    print()
    delete_theme(idx)


def list_themes():
    """List themes in themes.h, numbered.  Returns the names list."""
    if not os.path.exists(TARGET):
        print(f"ERROR: {TARGET} not found.")
        sys.exit(1)
    with open(TARGET, 'r', encoding='utf-8') as f:
        content = f.read()
    names, _, _ = _parse_names(content)
    if not names:
        print("No themes found.")
        return names
    print()
    print(f"Themes in file ({len(names)}):")
    print()
    for i, name in enumerate(names):
        print(f"[{i}] {name}")
    return names


def delete_theme(index):
    index = int(index)
    if not os.path.exists(TARGET):
        print(f"ERROR: {TARGET} not found.")
        sys.exit(1)
    with open(TARGET, 'r', encoding='utf-8') as f:
        content = f.read()

    names, _, _ = _parse_names(content)
    entries, _ = _parse_entries(content)

    if index < 0 or index >= len(names):
        print(f"ERROR: Index {index} out of range (0-{len(names)-1}).")
        sys.exit(1)

    if len(names) <= 1:
        print("ERROR: Cannot delete the last remaining theme.")
        sys.exit(1)

    if len(names) != len(entries):
        print(f"ERROR: Name/entry count mismatch ({len(names)} names, {len(entries)} entries).")
        sys.exit(1)

    target_name = names[index]

    print(f"About to delete: [{index}] {target_name}  (from {os.path.basename(TARGET)})")
    try:
        reply = input("This cannot be undone. Continue? [y/N] ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        print()
        reply = ''
    if reply not in ('y', 'yes'):
        print("Cancelled. Nothing was changed.")
        return False

    # Remove name from names list
    del names[index]

    # Remove the entry block.  _parse_entries() runs its ^ against a slice, so for any
    # entry after the first it also swallows the previous line's newline: normalise that
    # start, then consume the entry's own line terminator, so the neighbours end up on
    # adjacent lines instead of leaving a blank one behind or joining two lines.
    entry_start, entry_end = entries[index]
    if content[entry_start] == '\n':
        entry_start += 1
    if entry_end < len(content) and content[entry_end] == '\n':
        entry_end += 1
    content = content[:entry_start] + content[entry_end:]

    # Fix up: ensure no double-newlines after removal (max 1 blank line between entries)
    content = re.sub(r'\n{4,}', '\n\n\n', content)

    # Rebuild _themeNames in the content, keeping the file's own trailing-comma
    # style so a delete rewrites only the removed name's line.
    nm = re.search(r'const char _themeNames\[\]\[\d+\] PROGMEM = \{', content)
    if nm:
        be = content.find('\n};', nm.end())
        if be != -1:
            keeps_trailing_comma = content[nm.end():be].rstrip().endswith(',')
            lines = [f'    "{n}",' for n in names]
            if not keeps_trailing_comma:
                lines[-1] = lines[-1][:-1]
            new_names_block = f'const char _themeNames[][64] PROGMEM = {{\n' + '\n'.join(lines) + '\n};'
            content = content[:nm.start()] + new_names_block + content[be + 1:]

    content = re.sub(r'\};(\};)+', '};', content)
    with open(TARGET, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f"Deleted [{index}] {target_name}")
    list_themes()
    return True

def main():
    argv = sys.argv[1:]
    if not argv:
        interactive_delete()
    elif argv[0] in ('-h', '--help'):
        print(__doc__)
    elif len(argv) == 1 and argv[0].lstrip('-').isdigit():
        delete_theme(argv[0])  # asks for confirmation, always
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
