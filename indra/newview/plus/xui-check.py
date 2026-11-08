"""Checks that the translations of the XUI files Charis changes still apply.

A translated XUI file (skins/default/xui/<lang>/X.xml) only overrides en/X.xml
where its elements match en's: LLXMLNode::updateNode pairs each child with
the child of the matched parent that has the same name (or, without a name,
the same value). A translated element with no such match is silently ignored
and its text stays English. Wrapping en's settings in a new container
(<!-- plus --> blocks) without wrapping each language's the same way is the
usual way to break it.

Upstream's translations already hold many elements en no longer has; those
are not ours to fix. So each translated element that matches nothing now is
reported only if it matched in upstream's version of the two files (the
merge base with upstream/develop), or is not in upstream's translation at all
(we added it). The newest official release merged in (the tag
Second_Life_Release#* that HEAD contains) is a second upstream version: an
element it made stale (en dropped it there, as 26.5 of 2026-10-22 did the Visual
Leak Detector item) is upstream's too.

Checked: the files of skins/default/xui/en that carry a <!-- plus --> marker,
and our own files (*plus*.xml), against each language's file of the same name.

Usage: python -I indra/newview/plus/xui-check.py [--base REV] [--all]
  run from anywhere in the repository (plus-hooks.sh calls it)
  --base REV  upstream's version to compare with (default: the merge base of
              HEAD and upstream/develop, and the newest release tag in HEAD)
  --all       every en file, not only ours
Exit status 1 when a translation no longer applies.
"""
import os, subprocess, sys
import xml.etree.ElementTree as ET

XUI = 'indra/newview/skins/default/xui'


def git(top, *args):
    r = subprocess.run(['git', '-C', top] + list(args), capture_output=True)
    return r.stdout if r.returncode == 0 else None


def key(element):
    """What updateNode matches a child by: its name, else its value."""
    return element.get('name') or element.get('value') or ''


def identity(element):
    """A translated element, recognisable across versions whatever its parents."""
    return (element.tag, key(element), tuple(sorted(element.attrib.items())), (element.text or '').strip())


def walk(en, translated, path, matched, unmatched):
    """Sorts the translated elements (recursively) by whether updateNode finds
    their counterpart in en: matched as identities, unmatched as (path, identity)."""
    children = {}
    for child in en:
        if isinstance(child.tag, str) and key(child):
            children.setdefault(key(child), child)
    for child in translated:
        if not isinstance(child.tag, str):
            continue    # comments
        k = key(child)
        where = '%s/%s[%s]' % (path, child.tag, k or 'no name')
        match = children.get(k) if k else None
        if match is None:
            unmatched.append((where, identity(child)))
            continue
        matched.add(identity(child))
        walk(match, child, where, matched, unmatched)


def parse(data):
    try:
        return ET.fromstring(data)
    except ET.ParseError:
        return None


def is_ours(path):
    if 'plus' in os.path.basename(path):
        return True
    with open(path, encoding='utf-8', errors='replace') as f:
        return '<!-- plus' in f.read()


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    argv = sys.argv[1:]
    top = git(os.path.dirname(os.path.abspath(__file__)), 'rev-parse', '--show-toplevel').decode().strip()
    base = argv[argv.index('--base') + 1] if '--base' in argv else None
    bases = [base] if base else []
    if base is None:
        out = git(top, 'merge-base', 'HEAD', 'upstream/develop')
        base = out.decode().strip() if out else None
        if not base:
            print('xui-check: no upstream/develop to compare with (git fetch upstream, or --base REV); skipped')
            return
        bases = [base]
        out = git(top, 'describe', '--tags', '--abbrev=0', '--match', 'Second_Life_Release#*', 'HEAD')
        release = out.decode().strip() if out else ''
        if release:
            bases.append(release)
    xui = os.path.join(top, XUI)
    en_dir = os.path.join(xui, 'en')
    langs = sorted(d for d in os.listdir(xui) if d != 'en' and os.path.isdir(os.path.join(xui, d)))
    files = sorted(f for f in os.listdir(en_dir)
                   if f.endswith('.xml') and ('--all' in argv or is_ours(os.path.join(en_dir, f))))
    problems = 0
    checked = 0
    for name in files:
        with open(os.path.join(en_dir, name), 'rb') as f:
            en = parse(f.read())
        if en is None:
            print('en/%s: not well-formed' % name)
            problems += 1
            continue
        base_ens = [(b, parse(git(top, 'show', '%s:%s/en/%s' % (b, XUI, name)) or b'')) for b in bases]
        for lang in langs:
            path = os.path.join(xui, lang, name)
            if not os.path.exists(path):
                continue
            checked += 1
            with open(path, 'rb') as f:
                translated = parse(f.read())
            if translated is None:
                print('%s/%s: not well-formed' % (lang, name))
                problems += 1
                continue
            matched, unmatched = set(), []
            walk(en, translated, translated.tag, matched, unmatched)
            # upstream's stale elements: unmatched in one of its versions (and not matched there)
            upstream_stale = set()
            for b, base_en in base_ens:
                base_matched, base_unmatched = set(), []
                base_translated = parse(git(top, 'show', '%s:%s/%s/%s' % (b, XUI, lang, name)) or b'')
                if base_en is not None and base_translated is not None:
                    walk(base_en, base_translated, base_translated.tag, base_matched, base_unmatched)
                upstream_stale |= {ident for _, ident in base_unmatched} - base_matched
            for where, ident in unmatched:
                if ident not in upstream_stale:
                    print('%s/%s: %s matches nothing in en (not applied)' % (lang, name, where))
                    problems += 1
    print('== xui: %d en file(s), %d translation(s) checked against %s, %d problem(s)' %
          (len(files), checked, ', '.join(b[:10] if len(b) == 40 else b for b in bases), problems))
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
