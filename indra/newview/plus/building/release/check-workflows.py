#!/usr/bin/env python3
r"""Checks the GitHub Actions workflows of this repository (.github/workflows/*.yml, *.yaml)
against the rules of the release workflow, before they are published or changed.

  python -I check-workflows.py [--allow-todo] [FILE ...]

  --allow-todo  an action not pinned yet (@TODO-...) is a WARN, not an NG
                (while the pins are still to be filled in before publishing)
  FILE          the workflow files (default: everything in .github/workflows: a
                .yml or .yaml file is checked, any other file is an NG; a YAML file
                directly in .github, such as dependabot.yml, is a WARN to look at)

Rules (each finding is an NG unless said otherwise):
  syntax      the files are read with a small parser of the block style these
              files use (mappings, sequences, quoted and plain scalars, | and >
              blocks, [] and {} on one line); no tabs, no duplicate keys, no
              quoted keys, no escape but \\ and \" in a double-quoted value, no
              plain scalar with ": " or a leading indicator. Not a full YAML
              parser: GitHub and actionlint remain the final word
  triggers    only push (tags only: no branches or paths), workflow_dispatch and
              workflow_call; never pull_request, pull_request_target,
              workflow_run, issue_comment or schedule
  permissions a workflow with its own triggers starts with "permissions: {}" and
              every job states its own; write only for contents, id-token and
              attestations, and only in a job that runs steps (never in a job that
              calls a workflow) and that has an environment or needs (directly or
              not) a job that has one (the approval); a workflow_call-only workflow
              grants nothing above read
  gate        on the way to a write job: no always(), failure() or cancelled() in
              an if:, no continue-on-error; no continue-on-error in a step of the
              approval job; an environment is the literal "release"
  actions     only the GitHub actions the release uses (ALLOWED_ACTIONS) or a
              workflow of this repository (./.github/workflows/...), pinned by a
              full commit SHA with the version in a comment; every uses: on a
              plain "uses: <ref>" line
  run         no ${{ }} expression inside a run: script (values go through env:)
  checkout    persist-credentials: false
  artifacts   upload-artifact never with overwrite (a name taken before makes the
              upload, and the run, fail); download-artifact with a name and none of
              run-id, github-token, repository, pattern, merge-multiple, artifact-ids
              (one artifact of this run, by name)
  jobs        runs-on is a GitHub-hosted runner (no self-hosted) and the job has
              timeout-minutes; no container: or services:; a called workflow is there
  secrets     none but secrets.GITHUB_TOKEN (in any case): no other secrets.X,
              secrets['X'], toJSON(secrets), no "secrets:" on a job (inherit or a list)
  git         the file is not ignored by .gitignore (build-* would hide it); a
              git that cannot tell (not a checkout) is a WARN
Exit status: 0 = no NG, 1 = an NG (or a file cannot be read), 2 = wrong arguments.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))

ALLOWED_TRIGGERS = {'push', 'workflow_dispatch', 'workflow_call'}
PERMISSION_SCOPES = {'actions', 'attestations', 'checks', 'contents', 'deployments', 'id-token', 'issues',
                     'discussions', 'packages', 'pages', 'pull-requests', 'repository-projects',
                     'security-events', 'statuses', 'models'}
USES_LINE = re.compile(r'^\s*(?:-\s+)?uses:\s*(\S+)\s*(?:#\s*(.*))?$')
# The actions the release workflows use, and nothing else (github-script and the like
# would run expressions as code).
ALLOWED_ACTIONS = {'actions/checkout', 'actions/setup-python', 'actions/setup-dotnet', 'actions/cache/restore',
                   'actions/cache/save', 'actions/upload-artifact', 'actions/download-artifact',
                   'actions/attest-build-provenance'}
WRITE_SCOPES = {'contents', 'id-token', 'attestations'}
# download-artifact takes one artifact of this run by its name: not another run's (run-id,
# github-token), not several (pattern, merge-multiple), not by id.
DOWNLOAD_ARTIFACT_REFUSED = {'run-id', 'github-token', 'repository', 'pattern', 'merge-multiple', 'artifact-ids'}
PINNED = re.compile(r'^([A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+)@[0-9a-f]{40}$')
TODO_PIN = re.compile(r'^([A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+)@TODO\S*$')
EXPRESSION = re.compile(r'\$\{\{(.*?)\}\}', re.S)
LOCAL_WORKFLOW = re.compile(r'^\./\.github/workflows/[A-Za-z0-9_.-]+\.ya?ml$')
PLAIN_START_BAD = set('[]{}&*!|>%@`"\'')


class YamlError(Exception):
    pass


# ---- a parser for the block style of these files ------------------------------------------

def strip_comment(text):
    """text without a trailing " # comment" (outside quotes)."""
    quote = None
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = None
        elif ch in '\'"' and (i == 0 or text[i - 1] in ' [{,:'):
            quote = ch
        elif ch == '#' and (i == 0 or text[i - 1] == ' '):
            return text[:i].rstrip()
    return text.rstrip()


def split_key(text, lineno):
    """'key: rest' -> (key, rest); None when the text is not a mapping entry."""
    m = re.match(r'^("(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\'|[^\'"\s#][^:#]*?)\s*:(?:\s+(.*)|)$', text)
    if not m:
        return None
    key = m.group(1)
    if key[0] in '\'"':
        # A quoted key ("uses": ...) would slip past the line-based checks; these files never need one.
        raise YamlError('line %d: a quoted key (write keys plain): %s' % (lineno, key))
    return key, (m.group(2) or '').strip()


def scalar(text, lineno):
    if text == '':
        return None
    if text in ('{}', '[]'):
        return {} if text == '{}' else []
    if text[0] == "'":
        if len(text) < 2 or text[-1] != "'":
            raise YamlError('line %d: unterminated quote: %s' % (lineno, text))
        return text[1:-1].replace("''", "'")
    if text[0] == '"':
        inner = text[1:-1]
        if len(text) < 2 or text[-1] != '"' or re.search(r'(?<!\\)(\\\\)*\\$', inner):
            raise YamlError('line %d: unterminated quote: %s' % (lineno, text))
        # Only \\ and \" are read here: any other escape (\x24, $, \e ...) could spell
        # "${{" or "secrets" past the checks, so it is refused.
        if '\\' in re.sub(r'\\[\\"]', '', inner):
            raise YamlError('line %d: an escape other than \\\\ or \\" in a double-quoted value: %s' % (lineno, text))
        return re.sub(r'\\([\\"])', r'\1', inner)
    if text[0] == '[' and text[-1] == ']':
        inner = text[1:-1].strip()
        return [scalar(part.strip(), lineno) for part in inner.split(',')] if inner else []
    if text[0] in PLAIN_START_BAD:
        raise YamlError('line %d: a plain value may not start with %r: %s' % (lineno, text[0], text))
    if ': ' in text or text.endswith(':'):
        raise YamlError('line %d: ": " in a plain value (quote it): %s' % (lineno, text))
    if text in ('true', 'false'):
        return text == 'true'
    if re.fullmatch(r'-?\d+', text):
        return int(text)
    return text


class Parser:
    def __init__(self, text):
        if '\t' in text:
            raise YamlError('line %d: a tab' % (text[:text.index('\t')].count('\n') + 1))
        self.lines = text.split('\n')
        self.i = 0

    def skip(self):
        """Moves past blank and comment-only lines; returns (indent, text, lineno) or None."""
        while self.i < len(self.lines):
            raw = self.lines[self.i]
            body = raw.strip()
            if body and not body.startswith('#'):
                return len(raw) - len(raw.lstrip(' ')), strip_comment(raw.strip()), self.i + 1
            self.i += 1
        return None

    def parse(self):
        line = self.skip()
        if line is None:
            raise YamlError('empty file')
        node = self.block(line[0])
        if self.skip() is not None:
            raise YamlError('line %d: unexpected indentation' % (self.i + 1))
        return node

    def block(self, indent):
        line = self.skip()
        return self.sequence(indent) if line[1] == '-' or line[1].startswith('- ') else self.mapping(indent)

    def block_scalar(self, parent_indent):
        """The lines of a | or > block after the current line (raw, dedented)."""
        out, block_indent = [], None
        while self.i < len(self.lines):
            raw = self.lines[self.i]
            if raw.strip():
                ind = len(raw) - len(raw.lstrip(' '))
                if ind <= parent_indent:
                    break
                block_indent = ind if block_indent is None else block_indent
                if ind < block_indent:
                    raise YamlError('line %d: less indented than the block above' % (self.i + 1))
                out.append(raw[block_indent:])
            else:
                out.append('')
            self.i += 1
        return '\n'.join(out).rstrip('\n') + '\n'

    def value(self, rest, indent, lineno):
        """The value after "key:" or "- " at indent (the current line is consumed)."""
        self.i += 1
        if re.fullmatch(r'[|>][+-]?', rest):
            return self.block_scalar(indent)
        if rest:
            return scalar(rest, lineno)
        nxt = self.skip()
        if nxt is None:
            return None
        if nxt[0] > indent:
            return self.block(nxt[0])
        if nxt[0] == indent and (nxt[1] == '-' or nxt[1].startswith('- ')):
            return self.sequence(indent)
        return None

    def mapping(self, indent):
        node = {}
        while True:
            line = self.skip()
            if line is None or line[0] < indent:
                return node
            ind, text, lineno = line
            if ind > indent:
                raise YamlError('line %d: unexpected indentation' % lineno)
            if text == '-' or text.startswith('- '):
                return node
            kv = split_key(text, lineno)
            if kv is None:
                raise YamlError('line %d: not "key: value": %s' % (lineno, text))
            key, rest = kv
            if key in node:
                raise YamlError('line %d: duplicate key %r' % (lineno, key))
            node[key] = self.value(rest, indent, lineno)

    def sequence(self, indent):
        node = []
        while True:
            line = self.skip()
            if line is None or line[0] != indent or not (line[1] == '-' or line[1].startswith('- ')):
                if line is not None and line[0] > indent:
                    raise YamlError('line %d: unexpected indentation' % line[2])
                return node
            ind, text, lineno = line
            rest = text[1:].lstrip(' ')
            if rest and split_key(rest, lineno) is not None and rest[0] not in '\'"[{':
                # "- key: value": a mapping whose keys line up with "key"
                col = ind + (len(text) - len(rest))
                raw = self.lines[self.i]
                self.lines[self.i] = ' ' * col + raw.lstrip(' ')[len(text) - len(rest):]
                node.append(self.mapping(col))
            else:
                node.append(self.value(rest, indent, lineno))


# ---- the rules ------------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.ng = 0

    def say(self, kind, path, text):
        if kind == 'NG':
            self.ng += 1
        try:
            shown = os.path.relpath(path, SOURCE)
        except ValueError:      # another drive
            shown = path
        print('%-4s %s: %s' % (kind, shown.replace(os.sep, '/'), text))


def check_triggers(rep, path, on):
    if not isinstance(on, dict) or not on:
        rep.say('NG', path, '"on" is not a mapping of triggers')
        return set()
    names = set(on)
    for bad in sorted(names - ALLOWED_TRIGGERS):
        rep.say('NG', path, 'trigger %s is not allowed (only %s)' % (bad, ', '.join(sorted(ALLOWED_TRIGGERS))))
    push = on.get('push')
    if 'push' in on and (not isinstance(push, dict) or set(push) != {'tags'}):
        rep.say('NG', path, 'push must have tags only (no branches, paths or bare push)')
    return names


def job_permissions(rep, path, name, job):
    """The job's permissions as {scope: level}, after checking the names and levels."""
    perms = job.get('permissions')
    if perms is None:
        return None
    if not isinstance(perms, dict):
        rep.say('NG', path, 'job %s: permissions is not a mapping (no read-all / write-all)' % name)
        return {}
    for scope, level in perms.items():
        if scope not in PERMISSION_SCOPES or level not in ('read', 'write', 'none'):
            rep.say('NG', path, 'job %s: permission %s: %s is not known' % (name, scope, level))
    return perms


def gated(jobs, name, seen=None):
    """True when the job has an environment or needs, directly or not, a job that has one."""
    seen = set() if seen is None else seen
    if name in seen or not isinstance(jobs.get(name), dict):
        return False
    seen.add(name)
    job = jobs[name]
    if job.get('environment'):
        return True
    needs = job.get('needs') or []
    return any(gated(jobs, n, seen) for n in ([needs] if isinstance(needs, str) else needs))


def check_permissions(rep, path, wf, jobs, own_triggers):
    if own_triggers and wf.get('permissions') != {}:
        rep.say('NG', path, 'the workflow does not start with "permissions: {}"')
    if not own_triggers and wf.get('permissions') not in (None, {}):
        rep.say('NG', path, 'a workflow_call-only workflow states workflow-wide permissions')
    for name, job in jobs.items():
        if not isinstance(job, dict):
            continue
        perms = job_permissions(rep, path, name, job)
        if perms is None:
            if own_triggers:
                rep.say('NG', path, 'job %s states no permissions' % name)
            continue
        writes = sorted(s for s, level in perms.items() if level == 'write')
        if not writes:
            continue
        if not own_triggers:
            rep.say('NG', path, 'job %s of a workflow_call-only workflow asks for write (%s)' % (name, ', '.join(writes)))
            continue
        for scope in writes:
            if scope not in WRITE_SCOPES:
                rep.say('NG', path, 'job %s: write for %s (only %s)' % (name, scope, ', '.join(sorted(WRITE_SCOPES))))
        if 'uses' in job:
            rep.say('NG', path, 'job %s passes write (%s) to a called workflow' % (name, ', '.join(writes)))
        if not gated(jobs, name):
            rep.say('NG', path, 'job %s has write (%s) without an environment before it (the approval)'
                    % (name, ', '.join(writes)))


def gate_chain(jobs):
    """The jobs with write permissions and every job they need, directly or not."""
    chain, todo = set(), [n for n, j in jobs.items() if isinstance(j, dict) and isinstance(j.get('permissions'), dict)
                          and 'write' in j['permissions'].values()]
    while todo:
        name = todo.pop()
        if name in chain or not isinstance(jobs.get(name), dict):
            continue
        chain.add(name)
        needs = jobs[name].get('needs') or []
        todo.extend([needs] if isinstance(needs, str) else needs)
    return chain


def check_gate(rep, path, jobs):
    """Nothing lets a write job run when the approval did not pass: no always(), failure() or
    cancelled() in the if: of a job on the way, no continue-on-error there or in a step of the
    approval, and the environment is the literal "release"."""
    for name in sorted(gate_chain(jobs)):
        job = jobs[name]
        if re.search(r'\b(always|failure|cancelled)\s*\(', str(job.get('if', '')), re.I):
            rep.say('NG', path, 'job %s (on the way to a write job) runs regardless of the jobs before it: if: %s'
                    % (name, job['if']))
        if job.get('continue-on-error') not in (None, False):
            rep.say('NG', path, 'job %s (on the way to a write job) has continue-on-error' % name)
    for name, job in jobs.items():
        if not isinstance(job, dict) or 'environment' not in job:
            continue
        env = job['environment']
        env_name = env.get('name') if isinstance(env, dict) else env
        if env_name != 'release':
            rep.say('NG', path, 'job %s: environment %r (only the literal release)' % (name, env))
        for n, step in enumerate(job.get('steps') or [], 1):
            if isinstance(step, dict) and step.get('continue-on-error') not in (None, False):
                rep.say('NG', path, 'job %s step %d: continue-on-error in the approval job' % (name, n))


def check_steps(rep, path, name, job):
    """Returns the uses: of the job and its steps (for check_pins)."""
    for key in ('container', 'services'):
        if key in job:
            rep.say('NG', path, 'job %s has %s: (only plain GitHub-hosted runners)' % (name, key))
    if 'runs-on' in job and not (isinstance(job['runs-on'], str)
                                 and re.fullmatch(r'(ubuntu|windows|macos)-[0-9a-z.-]+', job['runs-on'])):
        rep.say('NG', path, 'job %s: runs-on %r is not a GitHub-hosted runner (no self-hosted)' % (name, job['runs-on']))
    if 'secrets' in job:
        rep.say('NG', path, 'job %s passes secrets to a called workflow (%r)' % (name, job['secrets']))
    if 'uses' in job:
        if not LOCAL_WORKFLOW.match(str(job['uses'])):
            rep.say('NG', path, 'job %s calls %s (only workflows of this repository)' % (name, job['uses']))
        elif not os.path.isfile(os.path.join(SOURCE, *job['uses'][2:].split('/'))):
            rep.say('NG', path, 'job %s calls %s, which is not there' % (name, job['uses']))
        return [str(job['uses'])]
    if 'runs-on' not in job:
        rep.say('NG', path, 'job %s has neither runs-on nor uses' % name)
    if not isinstance(job.get('timeout-minutes'), int):
        rep.say('NG', path, 'job %s has no timeout-minutes' % name)
    uses_refs = []
    for n, step in enumerate(job.get('steps') or [], 1):
        if not isinstance(step, dict):
            rep.say('NG', path, 'job %s step %d is not a mapping' % (name, n))
            continue
        if 'uses' in step:
            uses_refs.append(str(step['uses']))
        label = 'job %s step %d (%s)' % (name, n, step.get('name', '?'))
        if isinstance(step.get('run'), str) and '${{' in step['run']:
            rep.say('NG', path, '%s: ${{ }} inside run: (pass it through env:)' % label)
        uses = str(step.get('uses', ''))
        action, given = uses.split('@')[0], step.get('with') or {}
        if not isinstance(given, dict):
            rep.say('NG', path, '%s: with is not a mapping' % label)
            given = {}
        # GitHub reads input names without regard to case: compare them in lower case, and
        # refuse two that are the same name then (which of them wins is not written down).
        inputs = {}
        for key, value in given.items():
            if str(key).lower() in inputs:
                rep.say('NG', path, '%s: the input %s is there twice (ignoring case)' % (label, key))
            inputs[str(key).lower()] = value
        if action == 'actions/checkout':
            if inputs.get('persist-credentials') is not False:
                rep.say('NG', path, '%s: actions/checkout without persist-credentials: false' % label)
        elif action == 'actions/upload-artifact':
            if inputs.get('overwrite') not in (None, False):
                rep.say('NG', path, '%s: upload-artifact with overwrite (an artifact is never replaced)' % label)
        elif action == 'actions/download-artifact':
            for key in sorted(DOWNLOAD_ARTIFACT_REFUSED & set(inputs)):
                rep.say('NG', path, '%s: download-artifact with %s (one artifact of this run, by name)' % (label, key))
            if not inputs.get('name'):
                rep.say('NG', path, '%s: download-artifact without name (one artifact of this run, by name)' % label)
    return uses_refs


def check_secrets(rep, path, text):
    """Only secrets.GITHUB_TOKEN may appear in an expression."""
    for m in EXPRESSION.finditer(text):
        # Context names are case-insensitive in expressions (SECRETS.FOO is secrets.FOO)
        expr = re.sub(r'\bsecrets\.GITHUB_TOKEN\b', '', m.group(1), flags=re.I)
        if re.search(r'\bsecrets\b', expr, re.I):
            lineno = text[:m.start()].count('\n') + 1
            rep.say('NG', path, 'line %d: a secret other than GITHUB_TOKEN: ${{%s}}' % (lineno, m.group(1)))


def check_pins(rep, path, text, allow_todo, uses_refs):
    """The uses: lines (pins and version comments); every uses: the parser found must be on
    such a line (so nothing reaches GitHub unchecked)."""
    seen = []
    for lineno, line in enumerate(text.split('\n'), 1):
        m = USES_LINE.match(line)
        if not m:
            continue
        seen.append(m.group(1))
        ref, comment = m.group(1), (m.group(2) or '').strip()
        if LOCAL_WORKFLOW.match(ref):
            continue
        pinned, todo = PINNED.match(ref), TODO_PIN.match(ref)
        action = (pinned or todo).group(1) if (pinned or todo) else ref.split('@')[0]
        if action not in ALLOWED_ACTIONS:
            rep.say('NG', path, 'line %d: %s is not one of the allowed actions (%s)'
                    % (lineno, action, ', '.join(sorted(ALLOWED_ACTIONS))))
        elif pinned:
            if not re.search(r'\bv\d', comment):
                rep.say('NG', path, 'line %d: %s has no version comment (# vN.N.N)' % (lineno, ref))
        elif todo:
            rep.say('WARN' if allow_todo else 'NG', path, 'line %d: %s is not pinned yet (%s)' % (lineno, ref, comment))
        else:
            rep.say('NG', path, 'line %d: %s is not pinned by a full commit SHA' % (lineno, ref))
    for ref in uses_refs:
        if ref not in seen:
            rep.say('NG', path, 'uses %r is not on a plain "uses: <ref>" line' % ref)


def check_ignored(rep, path):
    """A workflow git ignores would never reach the repository (.gitignore has build-*)."""
    try:
        r = subprocess.run(['git', '-C', SOURCE, 'check-ignore', '-q', '--', path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        rep.say('WARN', path, 'git not found: not checked against .gitignore')
        return
    if r.returncode == 0:
        rep.say('NG', path, 'git ignores this file (.gitignore): it would not be committed')
    elif r.returncode != 1:
        rep.say('WARN', path, 'git check-ignore could not tell (exit %d; not a checkout?)' % r.returncode)


def check_file(rep, path, allow_todo):
    check_ignored(rep, path)
    with open(path, encoding='utf-8') as f:
        text = f.read()
    try:
        wf = Parser(text).parse()
    except YamlError as e:
        rep.say('NG', path, 'syntax: %s' % e)
        return
    if not isinstance(wf, dict):
        rep.say('NG', path, 'not a mapping at the top')
        return
    triggers = check_triggers(rep, path, wf.get('on'))
    jobs = wf.get('jobs')
    if not isinstance(jobs, dict) or not jobs:
        rep.say('NG', path, 'no jobs')
        return
    check_permissions(rep, path, wf, jobs, triggers - {'workflow_call'})
    check_gate(rep, path, jobs)
    uses_refs = []
    for name, job in jobs.items():
        if not isinstance(job, dict):
            rep.say('NG', path, 'job %s is not a mapping' % name)
            continue
        uses_refs += check_steps(rep, path, name, job)
    check_secrets(rep, path, text)
    check_pins(rep, path, text, allow_todo, uses_refs)
    rep.say('OK', path, 'read (%d jobs)' % len(jobs))


def default_files(rep):
    """Every file of .github/workflows (only .yml and .yaml belong there); the YAML files
    directly in .github (dependabot.yml and the like) are a WARN to look at by hand."""
    github = os.path.join(SOURCE, '.github')
    files = []
    # os.scandir: dot files too (glob('*') leaves them out)
    workflows = os.path.join(github, 'workflows')
    for entry in sorted(os.scandir(workflows), key=lambda e: e.name) if os.path.isdir(workflows) else ():
        if entry.is_file(follow_symlinks=False) and entry.name.endswith(('.yml', '.yaml')) \
                and not entry.name.startswith('.'):
            files.append(entry.path)
        else:
            rep.say('NG', entry.path, 'not a workflow file (.yml or .yaml) in .github/workflows')
    for entry in sorted(os.scandir(github), key=lambda e: e.name) if os.path.isdir(github) else ():
        if entry.is_file() and entry.name.endswith(('.yml', '.yaml')):
            rep.say('WARN', entry.path, 'a YAML file in .github that this check does not read: look at it by hand')
    return files


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--allow-todo', action='store_true')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    rep = Report()
    files = a.files or default_files(rep)
    if not files:
        print('no workflow files')
        return 1
    for path in files:
        try:
            check_file(rep, os.path.abspath(path), a.allow_todo)
        except OSError as e:
            rep.say('NG', path, str(e))
    print('WORKFLOW-CHECK: %s' % ('OK' if rep.ng == 0 else 'NG (%d)' % rep.ng))
    return 0 if rep.ng == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
