"""Desk check of the pose bridge's script (app_settings/plus/lslbridge/charis_bridge.lsl), research/336.

Not part of the build. Run:
  python -I lslbridge_lsl_check.py [script.lsl]   the shipped script (or another copy of it)
  python -I lslbridge_lsl_check.py --mutants      each built-in mutation of the shipped script (and of the
                                                  viewer's lslbridge.cpp, llappearancemgr.cpp's hooks, plus/outfitbackup,
                                                  the poser's line and their texts) must fail a check the shipped one passes

1. Agreement: the script and the viewer use the same version, channel, tags, actions and limits
   (lslbridgeproto.h / .cpp); the viewer waits for a new URL at least the script's gap between announcements plus
   URL_GRANT_MARGIN (lslbridge.cpp ANSWER_SECONDS >= ANNOUNCE_GAP + 10); the viewer says hello only through
   helloWhenClean(), after the bridge's contents were seen holding no other script for the URL held (cleanOf).
   The viewer's lslbridge.cpp is read for its checks of the contents and the lines: cleanOf set only by a clean
   answer to our own ask and cleared when another script is found (the URL held dropped then), by another URL and
   by a new attempt; the contents asked of the simulator (the listener, dirtyInventory(), then requestInventory()),
   an answer to a request already on the way asked once more (Again, with its own time in CheckContents, never a
   third), an answer left for the idle after a newer ask dropped (its number); a line only from the root part of
   the worn bridge (fully audible, the bridge item); its pose lines only with cleanOf, in the form, not naming
   ourselves and not while Off, and dropped unread (not shown) when they do not pass, also when the bridge is known
   only by its id (the speaker's object not seen, or not confirmed as the worn bridge); the URL lines of any other
   object dropped unread; in CheckContents an answer for an object no longer worn adds no script (the answer is the
   worn object's by its id; a take-off there is watched and is a repair); only the llOwnerSay lines of our own
   objects looked at; before the bridge item is known, a worn object taken as the bridge by its name, its place and
   its point only; no redirect followed; a pose sent only after the hello; our script known by its creator too; the
   script tries counted only when the script item went before it was put in, every making toward MAX_SCRIPT_MAKES,
   an upload that finds the bridge off taken as a take-off, the items of earlier makings removed before a new one is
   made; the functions that move the user's items to the trash, isOurBridge() and takeRewear() pinned as they read
   (PINNED, with takeOffOtherBridges() and watchLateWear()); the forgiving of the counts, the repairs' and retries'
   limits and the other limits as numbers, ensure() from Off only, the setting's gate at every way in (on by default,
   off in a safe start, research/353), the failures shown only when the person turned the bridge on in this session,
   the bridge made only once the login's outfit is on, asked for only with an attachment place to spare (else Failed
   NoSlot, tried again when the attachments leave a place), made anew by Auto repairs only when the person turned it on; failedForGood() and the
   poser's line for it (en, ja); the outfit's hooks in llappearancemgr.cpp (not saved into an outfit, no change of it,
   kept in a new COF only with a place, the person's attachments first; tried again once a place is free) and in
   plus/outfitbackup (out of its copy, compare and placements; the worn one kept by the same rule); OTHER_MUTANTS break
   them, the poser and the texts; a wear asked
   for that comes after the bridge stopped taken off again, its note gone once the bridge is seen worn; the makings
   from 0 at the hello; the bridge item found by its exact name; our scripts only judged; a library object copied
   only with its permissions; late post results dropped; "ok" the only good answer; dropUrl(), leaveSteps(),
   takeOffWhileOff(), classify(), onRefusedAnnouncement(), stop() and helloWhenClean() pinned; the epoch of every
   inventory, folder and upload callback; Silent repaired only where scripts run and with a URL; another script
   version made anew; the script file's size; the rest of the limits as numbers; the script tries from 0 at the
   hello.
2. Lint: braces and brackets balance; every ll... function is one LSL has (the list below); every function
   called is defined; the events have LSL's argument types; no reserved word is used as a name; every
   upper-case name is a declared global or a known LSL constant; every name a function or event uses is its
   parameter, its own local or a global.
3. Behaviour: a small interpreter for the subset of LSL the script is written in (types integer, float,
   string, key, list; if / else / while / return; LSL's evaluation of && and || without short-circuit) runs
   the script's own text against a mock simulator: the URL and its token, the hello for every new URL, the
   announcements every ANNOUNCE_GAP and the bridge taking itself off (also when a hello comes between asking for
   the permission and getting it), grants for an older request or while not worn, the URL released when taken off,
   a refused URL, other scripts stopped at the start, the askers forgotten when worn again, the viewer's commands
   (also a hello of three fields, an empty string, a token before any URL) and the pose1 relay; the call
   (research/366): nothing is said before the owner's viewer calls (not the URL, not the token, not the no-URL line),
   a call is heard only from the owner avatar and only with the bridge object's own key in its text (what a dialog
   button of someone's object makes the owner say is at most 24 bytes: the bare text, another key or version, upper
   case and the owner's key do nothing; a link in the chat or a dialog button can make the owner say any text, so the
   call holds this bridge object's key, which is on no list while worn on a HUD point: a call is taken on a HUD point
   (31 to 38) only, and the bridge says nothing on the body either, called or not), answered at once and at most every PROBE_GAP (a flood of calls: one answer),
   a URL that comes later or a refused one told after the call, an answer not counted toward the announcements, the
   call forgotten when worn again, rezzed or taken off, the deadline PAIR_WAIT from the wear (not moved by a region
   change) after which the bridge takes itself off without having said anything, and a call while not worn or on the
   body ignored.
   The viewer's calls (lslbridge.cpp): a whisper of proto::probeText(object key) on proto::PROBE_CHANNEL sent as
   ChatFromViewer, not through the RLV filter; only while a step waits, with no URL held, MAX_PROBES in a step every
   PROBE_GAP_SECONDS once logged in and while the bridge (or the recorded item) is worn on a HUD point, a bridge worn on
   the body put on the HUD again by a repair (stepWear); an old version's bridge never worn, waited for a wear's time when
   its wear is on the way, its outfit link removed and the item watched (goneOld) for the ones that still come.
"""
import os
import re
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
NEWVIEW = os.path.normpath(os.path.join(HERE, '..', '..'))
SHIPPED = os.path.join(NEWVIEW, 'app_settings', 'plus', 'lslbridge', 'charis_bridge.lsl')
PROTO_H = os.path.join(HERE, 'lslbridgeproto.h')
PROTO_CPP = os.path.join(HERE, 'lslbridgeproto.cpp')
BRIDGE_CPP = os.path.join(HERE, 'lslbridge.cpp')
SETTINGS_XML = os.path.join(NEWVIEW, 'app_settings', 'settings_plus.xml')
# research/353 (the pre-merge review): the outfit's hooks in the official llappearancemgr.cpp, the poser's line for a
# bridge that will not try again, and their texts.
APPEARANCE_CPP = os.path.join(NEWVIEW, 'llappearancemgr.cpp')
BACKUP_CPP = os.path.join(NEWVIEW, 'plus', 'outfitbackup', 'outfitbackup.cpp')
POSER_CPP = os.path.join(NEWVIEW, 'plus', 'poser', 'fsfloaterposer.cpp')
POSER_XML = {lang: os.path.join(NEWVIEW, 'skins', 'default', 'xui', lang, 'floater_fs_poser.xml') for lang in ('en', 'ja')}
NOTIFICATIONS_XML = {lang: os.path.join(NEWVIEW, 'skins', 'default', 'xui', lang, 'notifications.xml') for lang in ('en', 'ja')}
# A file's text as a mutant of OTHER_MUTANTS has it (only while that mutant is judged).
OVERRIDES = {}


def read(path):
    if path in OVERRIDES:
        return OVERRIDES[path]
    with open(path, encoding='utf-8') as f:
        return f.read()


# ================================================================ the LSL subset

TYPES = {'integer', 'float', 'string', 'key', 'list'}
ALL_TYPES = TYPES | {'vector', 'rotation', 'quaternion'}
KEYWORDS = {'if', 'else', 'while', 'for', 'do', 'return', 'jump', 'state', 'default', 'event', 'print'}
RESERVED = KEYWORDS | ALL_TYPES | {'TRUE', 'FALSE'}
EVENTS = {
    'state_entry': [], 'on_rez': ['integer'], 'attach': ['key'], 'changed': ['integer'],
    'http_request': ['key', 'string', 'string'], 'listen': ['integer', 'string', 'key', 'string'], 'timer': [],
    'run_time_permissions': ['integer'],
}
NULL_KEY = '00000000-0000-0000-0000-000000000000'
CONSTANTS = {
    'TRUE': 1, 'FALSE': 0, 'NULL_KEY': None, 'URL_REQUEST_GRANTED': 'URL_REQUEST_GRANTED',
    'URL_REQUEST_DENIED': 'URL_REQUEST_DENIED', 'OBJECT_ATTACHED_POINT': 19, 'INVENTORY_SCRIPT': 10,
    'PRIM_TEMP_ON_REZ': 4, 'LINK_THIS': -4, 'CHANGED_INVENTORY': 0x1, 'CHANGED_OWNER': 0x80,
    'CHANGED_REGION': 0x100, 'CHANGED_TELEPORT': 0x200, 'CHANGED_REGION_START': 0x400, 'PERMISSION_ATTACH': 0x20,
}
# The LSL library functions the script may call (the ones it uses and a few near them; any other ll... name is
# a typo the simulator's compiler would refuse).
LSL_FUNCTIONS = {
    'llAllowInventoryDrop', 'llGetTime', 'llResetTime', 'llList2Float', 'llShout', 'llDeleteSubList', 'llDeleteSubString', 'llDetachFromAvatar', 'llGenerateKey',
    'llGetAttached', 'llGetInventoryName', 'llGetKey', 'llGetInventoryNumber', 'llGetListLength', 'llGetObjectDetails',
    'llGetOwner', 'llGetOwnerKey', 'llGetScriptName', 'llGetSubString', 'llGetUnixTime', 'llHTTPResponse',
    'llList2Integer', 'llList2Key', 'llList2String', 'llListFindList', 'llListReplaceList', 'llListen',
    'llListenRemove', 'llOwnerSay', 'llParseStringKeepNulls', 'llRegionSayTo', 'llReleaseURL',
    'llRequestPermissions', 'llRequestSecureURL', 'llResetScript', 'llSetLinkPrimitiveParamsFast',
    'llSetScriptState', 'llSetTimerEvent', 'llStringLength', 'llSubStringIndex', 'llToLower',
}


class Key(str):
    """An LSL key: a string that is true when it is a valid, non-null UUID."""


def key_is_true(text):
    return bool(re.fullmatch(r'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}', text)) \
        and text != NULL_KEY


def truth(v):
    if isinstance(v, Key):
        return key_is_true(v)
    if isinstance(v, list):
        return len(v) > 0
    return bool(v)


TOKEN_RE = re.compile(r'''
    (?P<ws>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\])*")
  | (?P<num>0x[0-9a-fA-F]+|\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+|\d+)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>\+\+|--|\+=|-=|\*=|/=|==|!=|<=|>=|&&|\|\||<<|>>|[-+*/%<>=!&|^~(){}\[\],;])
''', re.S | re.X)


def tokenize(text):
    out, pos = [], 0
    while pos < len(text):
        m = TOKEN_RE.match(text, pos)
        if not m:
            raise SyntaxError('cannot read the script at %r' % text[pos:pos + 20])
        pos = m.end()
        kind = m.lastgroup
        if kind == 'ws':
            continue
        value = m.group()
        if kind == 'str':
            value = bytes(value[1:-1], 'utf-8').decode('unicode_escape')
        out.append((kind, value))
    out.append(('end', None))
    return out


class Parser:
    BINARY = [('||', '&&'), ('|',), ('^',), ('&',), ('==', '!='), ('<', '<=', '>', '>='), ('<<', '>>'), ('+', '-'),
              ('*', '/', '%')]

    def __init__(self, text):
        self.t = tokenize(text)
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def take(self, value=None):
        tok = self.t[self.i]
        if value is not None and tok[1] != value:
            raise SyntaxError('expected %r, found %r' % (value, tok[1]))
        self.i += 1
        return tok

    def script(self):
        globals_, functions, events = [], {}, {}
        while self.peek()[0] != 'end':
            if self.peek()[1] == 'default':
                self.take('default')
                self.take('{')
                while self.peek()[1] != '}':
                    name = self.take()[1]
                    params = self.params()
                    events[name] = (params, self.block())
                self.take('}')
            elif self.peek()[1] in TYPES and self.peek(2)[1] != '(':
                vtype = self.take()[1]
                name = self.take()[1]
                expr = None
                if self.peek()[1] == '=':
                    self.take('=')
                    expr = self.expr()
                self.take(';')
                globals_.append((vtype, name, expr))
            else:
                rtype = self.take()[1] if self.peek()[1] in TYPES else None
                name = self.take()[1]
                functions[name] = (rtype, self.params(), self.block())
        return globals_, functions, events

    def params(self):
        self.take('(')
        out = []
        while self.peek()[1] != ')':
            ptype = self.take()[1]
            pname = self.take()[1]
            out.append((ptype, pname))
            if self.peek()[1] == ',':
                self.take(',')
        self.take(')')
        return out

    def block(self):
        self.take('{')
        body = []
        while self.peek()[1] != '}':
            body.append(self.stmt())
        self.take('}')
        return ('block', body)

    def stmt(self):
        tok = self.peek()
        if tok[1] == '{':
            return self.block()
        if tok[1] == 'if':
            self.take('if')
            self.take('(')
            cond = self.expr()
            self.take(')')
            then = self.stmt()
            other = None
            if self.peek()[1] == 'else':
                self.take('else')
                other = self.stmt()
            return ('if', cond, then, other)
        if tok[1] == 'while':
            self.take('while')
            self.take('(')
            cond = self.expr()
            self.take(')')
            return ('while', cond, self.stmt())
        if tok[1] == 'return':
            self.take('return')
            expr = None if self.peek()[1] == ';' else self.expr()
            self.take(';')
            return ('return', expr)
        if tok[1] in TYPES and self.peek(1)[0] == 'id':
            vtype = self.take()[1]
            name = self.take()[1]
            expr = None
            if self.peek()[1] == '=':
                self.take('=')
                expr = self.expr()
            self.take(';')
            return ('local', vtype, name, expr)
        expr = self.expr()
        self.take(';')
        return ('expr', expr)

    def expr(self):
        left = self.binary(0)
        if self.peek()[1] in ('=', '+=', '-=', '*=', '/='):
            op = self.take()[1]
            if left[0] != 'name':
                raise SyntaxError('assignment to something that is not a name')
            return ('assign', op, left[1], self.expr())
        return left

    def binary(self, level):
        if level == len(self.BINARY):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek()[0] == 'op' and self.peek()[1] in self.BINARY[level]:
            op = self.take()[1]
            left = ('bin', op, left, self.binary(level + 1))
        return left

    def unary(self):
        tok = self.peek()
        if tok[0] == 'op' and tok[1] in ('!', '-', '~'):   # not the string "-"
            self.take()
            return ('un', tok[1], self.unary())
        if tok[0] == 'op' and tok[1] in ('++', '--'):
            self.take()
            name = self.take()[1]
            return ('pre', tok[1], name)
        if tok[0] == 'op' and tok[1] == '(' and self.peek(1)[1] in ALL_TYPES and self.peek(2)[1] == ')':
            self.take('(')
            ctype = self.take()[1]
            self.take(')')
            return ('cast', ctype, self.unary())
        return self.postfix()

    def postfix(self):
        node = self.primary()
        if self.peek()[1] in ('++', '--') and node[0] == 'name':
            return ('post', self.take()[1], node[1])
        return node

    def primary(self):
        kind, value = self.take()
        if kind == 'num':
            if value.startswith('0x'):
                return ('lit', int(value, 16))
            return ('lit', float(value) if ('.' in value or 'e' in value.lower()) else int(value))
        if kind == 'str':
            return ('lit', value)
        if value == '(':
            node = self.expr()
            self.take(')')
            return node
        if value == '[':
            items = []
            while self.peek()[1] != ']':
                items.append(self.expr())
                if self.peek()[1] == ',':
                    self.take(',')
            self.take(']')
            return ('list', items)
        if kind == 'id':
            if self.peek()[1] == '(':
                self.take('(')
                args = []
                while self.peek()[1] != ')':
                    args.append(self.expr())
                    if self.peek()[1] == ',':
                        self.take(',')
                self.take(')')
                return ('call', value, args)
            return ('name', value)
        raise SyntaxError('unexpected %r' % value)


class Return(Exception):
    def __init__(self, value):
        self.value = value


class Reset(Exception):
    pass


def default_of(vtype):
    return {'integer': 0, 'float': 0.0, 'string': '', 'key': Key(''), 'list': []}[vtype]


def convert(vtype, value):
    if vtype == 'key':
        return Key(str(value) if not isinstance(value, float) else '%f' % value)
    if vtype == 'string':
        if isinstance(value, float):
            return '%f' % value
        if isinstance(value, list):
            return ''.join(convert('string', v) for v in value)
        return str(value)
    if vtype == 'integer':
        if isinstance(value, str):
            m = re.match(r'\s*[-+]?\d+', value)
            return int(m.group()) if m else 0
        return int(value)
    if vtype == 'float':
        return float(value)
    if vtype == 'list':
        return value if isinstance(value, list) else [value]
    return value


def lsl_index(length, i):
    return i + length if i < 0 else i


def substring(text, start, end):
    n = len(text)
    s, e = lsl_index(n, start), lsl_index(n, end)
    if s <= e:
        return text[max(s, 0):max(min(e, n - 1) + 1, 0)]
    return text[:max(e + 1, 0)] + text[s:]


def delete_substring(text, start, end):
    n = len(text)
    s, e = lsl_index(n, start), lsl_index(n, end)
    if s <= e:
        return text[:max(s, 0)] + text[min(e, n - 1) + 1:]
    return text[e + 1:s]


class World:
    """The simulator around one worn bridge."""

    def __init__(self, owner):
        self.owner = owner
        self.point = 31
        self.time = 1000
        self.objects = {}              # key -> (owner, attachment point)
        self.said = []                 # llOwnerSay
        self.region_say = []           # (target, message)
        self.responses = {}            # request id -> (status, body)
        self.timer = 0.0
        self.url_requests = []         # pending llRequestSecureURL ids
        self.released = []
        self.scripts = ['#Charis Bridge v1.2']
        self.clock = 100.0             # llGetTime(): seconds the script has run (a float: the sub-second gaps)
        self.spoken = []               # (how, channel, target, message) of every llShout and llRegionSayTo
        self.listens = []              # (handle, channel, speaker filter) of every llListen, removed ones left out
        self.listen_count = 0
        self.stopped = []
        self.perms = []
        self.hold_perms = False        # True: run_time_permissions waits for deliver_perms() (something may come first)
        self.held = []
        self.detached = False
        self.temp_on_rez = False
        self.allow_drop = None         # the last llAllowInventoryDrop(); None: never called


class Script:
    def __init__(self, text, world):
        self.globals_def, self.functions, self.events = Parser(text).script()
        self.world = world
        self.queue = []
        self.reset_globals()

    def reset_globals(self):
        self.g = {}
        for vtype, name, expr in self.globals_def:
            self.g[name] = convert(vtype, self.eval(expr, {})) if expr else default_of(vtype)
        self.gtypes = {name: vtype for vtype, name, _ in self.globals_def}

    # -- running
    def fire(self, event, *args):
        if event not in self.events:
            return
        params, body = self.events[event]
        scope = {name: convert(ptype, a) for (ptype, name), a in zip(params, args)}
        types = {name: ptype for ptype, name in params}
        try:
            self.run(body, scope, types)
        except Return:
            pass
        except Reset:
            self.reset_globals()
            self.fire('state_entry')
        while self.queue:
            ev, ev_args = self.queue.pop(0)
            self.fire(ev, *ev_args)

    def run(self, node, scope, types):
        kind = node[0]
        if kind == 'block':
            for s in node[1]:
                self.run(s, scope, types)
        elif kind == 'if':
            if truth(self.eval(node[1], scope, types)):
                self.run(node[2], scope, types)
            elif node[3]:
                self.run(node[3], scope, types)
        elif kind == 'while':
            guard = 0
            while truth(self.eval(node[1], scope, types)):
                self.run(node[2], scope, types)
                guard += 1
                if guard > 10000:
                    raise RuntimeError('endless loop')
        elif kind == 'return':
            raise Return(self.eval(node[1], scope, types) if node[1] else None)
        elif kind == 'local':
            _, vtype, name, expr = node
            types[name] = vtype
            scope[name] = convert(vtype, self.eval(expr, scope, types)) if expr else default_of(vtype)
        elif kind == 'expr':
            self.eval(node[1], scope, types)

    def lookup(self, name, scope):
        if name in scope:
            return scope[name]
        if name in self.g:
            return self.g[name]
        if name in CONSTANTS:
            value = CONSTANTS[name]
            return Key(NULL_KEY) if name == 'NULL_KEY' else value
        raise NameError('unknown name %s' % name)

    def store(self, name, value, scope, types):
        if name in scope:
            scope[name] = convert(types.get(name, 'string'), value)
        elif name in self.g:
            self.g[name] = convert(self.gtypes[name], value)
        else:
            raise NameError('assignment to unknown name %s' % name)

    def eval(self, node, scope, types=None):
        types = types if types is not None else {}
        kind = node[0]
        if kind == 'lit':
            return node[1]
        if kind == 'name':
            return self.lookup(node[1], scope)
        if kind == 'list':
            return [self.eval(n, scope, types) for n in node[1]]
        if kind == 'cast':
            return convert(node[1], self.eval(node[2], scope, types))
        if kind == 'un':
            v = self.eval(node[2], scope, types)
            return {'!': lambda: int(not truth(v)), '-': lambda: -v, '~': lambda: ~v}[node[1]]()
        if kind in ('pre', 'post'):
            old = self.lookup(node[2], scope)
            new = old + (1 if node[1] == '++' else -1)
            self.store(node[2], new, scope, types)
            return new if kind == 'pre' else old
        if kind == 'assign':
            _, op, name, expr = node
            value = self.eval(expr, scope, types)
            if op == '+=':
                value = self.lookup(name, scope) + value
            elif op == '-=':
                value = self.lookup(name, scope) - value
            self.store(name, value, scope, types)
            return value
        if kind == 'bin':
            _, op, l, r = node
            # LSL evaluates both sides (no short-circuit), the right one first.
            b = self.eval(r, scope, types)
            a = self.eval(l, scope, types)
            if op == '&&':
                return int(truth(a) and truth(b))
            if op == '||':
                return int(truth(a) or truth(b))
            if op in ('==', '!='):
                if isinstance(a, list) or isinstance(b, list):
                    same = len(a) == len(b)   # LSL compares list lengths
                else:
                    same = str(a) == str(b) if isinstance(a, str) or isinstance(b, str) else a == b
                return int(same if op == '==' else not same)
            if op == '+':
                if isinstance(a, list) or isinstance(b, list):
                    return (a if isinstance(a, list) else [a]) + (b if isinstance(b, list) else [b])
                if isinstance(a, str) or isinstance(b, str):
                    return str(a) + str(b)
                return a + b
            ops = {'-': lambda: a - b, '*': lambda: a * b, '/': lambda: a // b if isinstance(a, int) and isinstance(b, int) else a / b,
                   '%': lambda: a % b, '<': lambda: int(a < b), '<=': lambda: int(a <= b), '>': lambda: int(a > b),
                   '>=': lambda: int(a >= b), '&': lambda: a & b, '|': lambda: a | b, '^': lambda: a ^ b,
                   '<<': lambda: a << b, '>>': lambda: a >> b}
            return ops[op]()
        if kind == 'call':
            name, args = node[1], [self.eval(a, scope, types) for a in node[2]]
            if name in self.functions:
                rtype, params, body = self.functions[name]
                fscope = {pname: convert(ptype, a) for (ptype, pname), a in zip(params, args)}
                ftypes = {pname: ptype for ptype, pname in params}
                try:
                    self.run(body, fscope, ftypes)
                except Return as r:
                    return convert(rtype, r.value) if rtype and r.value is not None else r.value
                return default_of(rtype) if rtype else None
            return self.library(name, args)
        raise RuntimeError('cannot evaluate %r' % (node,))

    def library(self, name, a):
        w = self.world
        if name == 'llGetSubString':
            return substring(a[0], a[1], a[2])
        if name == 'llDeleteSubString':
            return delete_substring(a[0], a[1], a[2])
        if name == 'llSubStringIndex':
            return a[0].find(a[1])
        if name == 'llStringLength':
            return len(a[0])
        if name == 'llToLower':
            return str(a[0]).lower()
        if name == 'llParseStringKeepNulls':
            parts, rest = [], a[0]
            seps = [s for s in a[1] if s]
            while True:
                hits = [(rest.find(s), s) for s in seps if rest.find(s) >= 0]
                if not hits:
                    parts.append(rest)
                    return parts
                at, sep = min(hits)
                parts.append(rest[:at])
                rest = rest[at + len(sep):]
        if name == 'llList2String':
            i = lsl_index(len(a[0]), a[1])
            return convert('string', a[0][i]) if 0 <= i < len(a[0]) else ''
        if name == 'llList2Integer':
            i = lsl_index(len(a[0]), a[1])
            return convert('integer', a[0][i]) if 0 <= i < len(a[0]) else 0
        if name == 'llList2Key':
            i = lsl_index(len(a[0]), a[1])
            return Key(str(a[0][i])) if 0 <= i < len(a[0]) else Key('')
        if name == 'llGetListLength':
            return len(a[0])
        if name == 'llListFindList':
            src, test = a[0], a[1]
            for i in range(len(src) - len(test) + 1):
                if all(type(x) is type(y) and x == y for x, y in zip(src[i:i + len(test)], test)):
                    return i
            return -1
        if name == 'llDeleteSubList':
            s, e = lsl_index(len(a[0]), a[1]), lsl_index(len(a[0]), a[2])
            return a[0][:s] + a[0][e + 1:]
        if name == 'llListReplaceList':
            s, e = lsl_index(len(a[0]), a[2]), lsl_index(len(a[0]), a[3])
            return a[0][:s] + a[1] + a[0][e + 1:]
        if name == 'llGetKey':
            return Key(THIS)
        if name == 'llGetOwner':
            return Key(w.owner)
        if name == 'llGetOwnerKey':
            return Key(w.objects[a[0]][0]) if a[0] in w.objects else Key(a[0])
        if name == 'llGetObjectDetails':
            if a[0] not in w.objects:
                return []
            return [w.objects[a[0]][1] for p in a[1] if p == CONSTANTS['OBJECT_ATTACHED_POINT']]
        if name == 'llGetAttached':
            return w.point
        if name == 'llGetUnixTime':
            return int(w.time)
        if name == 'llOwnerSay':
            w.said.append(a[0])
            return None
        if name == 'llRegionSayTo':
            w.region_say.append((str(a[0]), a[2]))
            w.spoken.append(('to', a[1], str(a[0]), a[2]))
            return None
        if name == 'llShout':
            w.spoken.append(('shout', a[0], '', a[1]))
            return None
        if name == 'llResetTime':
            w.clock = 0.0
            return None
        if name == 'llGetTime':
            return float(w.clock)
        if name == 'llList2Float':
            i = lsl_index(len(a[0]), a[1])
            return float(a[0][i]) if 0 <= i < len(a[0]) else 0.0
        if name == 'llHTTPResponse':
            w.responses[a[0]] = (a[1], a[2])
            return None
        if name == 'llSetTimerEvent':
            w.timer = a[0]
            return None
        if name == 'llRequestSecureURL':
            request = Key(str(uuid.uuid4()))
            w.url_requests.append(request)
            return request
        if name == 'llReleaseURL':
            w.released.append(a[0])
            return None
        if name == 'llGenerateKey':
            return Key(str(uuid.uuid4()))
        if name == 'llListen':
            w.listen_count += 1
            w.listens.append((w.listen_count, a[0], str(a[2])))
            return w.listen_count
        if name == 'llListenRemove':
            w.listens = [l for l in w.listens if l[0] != a[0]]
            return None
        if name == 'llAllowInventoryDrop':
            w.allow_drop = a[0]
            return None
        if name == 'llGetScriptName':
            return w.scripts[0]
        if name == 'llGetInventoryNumber':
            return len(w.scripts)
        if name == 'llGetInventoryName':
            return w.scripts[a[1]]
        if name == 'llSetScriptState':
            w.stopped.append(a[0])
            return None
        if name == 'llSetLinkPrimitiveParamsFast':
            w.temp_on_rez = True
            return None
        if name == 'llResetScript':
            raise Reset()
        if name == 'llRequestPermissions':
            w.perms.append(a[1])
            if w.point:
                (w.held if w.hold_perms else self.queue).append(('run_time_permissions', (a[1],)))
            return None
        if name == 'llDetachFromAvatar':
            w.detached = True
            w.point = 0
            return None
        raise NameError('the mock simulator has no %s' % name)


# ================================================================ the checks

def run_checks(lsl, proto_h, proto_cpp, bridge_cpp):
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    # ---- 1. agreement
    def lsl_global(name):
        m = re.search(r'^\s*(?:string|integer|float|list|key)\s+' + name + r'\s*=\s*(.+?);', lsl, re.M)
        return m.group(1).strip() if m else None

    def cpp_constant(text, name):
        m = re.search(name + r'\s*=\s*([0-9.]+)', text)
        return float(m.group(1)) if m else None

    version = re.search(r'BRIDGE_VERSION\[\]\s*=\s*"([^"]+)"', proto_h).group(1)
    check(lsl_global('VERSION') == '"%s"' % version, 'VERSION of the script is proto::BRIDGE_VERSION')
    probe_channel = re.search(r'PROBE_CHANNEL\s*=\s*(\d+);', proto_h)
    probe_text = re.search(r'PROBE_TEXT\[\]\s*=\s*("[^"]*");', proto_h)
    check(probe_channel is not None and lsl_global('PROBE_CHANNEL') == probe_channel.group(1),
          'the call channel of the script is proto::PROBE_CHANNEL')
    check(probe_text is not None and lsl_global('PROBE_TEXT') == probe_text.group(1), 'the call text of the script is proto::PROBE_TEXT')
    check(probe_channel is not None and 0 < int(probe_channel.group(1)) < 2147483647,
          'the call channel is positive (no avatar\'s chat shows it) and not the debug channel')
    check(lsl_global('POSE_CHANNEL') == '-184735220', 'the pose channel is Aurora\'s -184735220')
    check(lsl_global('POSE_MAX') == '130', 'pose1 messages of at most 130 bytes (Aurora)')
    # Sharing, reflect and push (research/374, 374b): the channels, limits and tags of the script and of the viewer.
    channels = {}
    for name in ('SHARE_CHANNEL', 'REFLECT_CHANNEL', 'PUSH_CHANNEL'):
        m = re.search(r'constexpr int\s+' + name + r'\s*=\s*(\d+);', proto_h)
        channels[name] = m.group(1) if m else None
        check(m is not None and lsl_global(name) == m.group(1), 'the %s of the script is proto::%s' % (name, name))
        check(m is not None and 0 < int(m.group(1)) < 2147483647,
              '%s is positive (no avatar\'s chat shows it) and not the debug channel' % name)
    check(len({channels['SHARE_CHANNEL'], channels['REFLECT_CHANNEL'], channels['PUSH_CHANNEL'], str(probe_channel.group(1))}) == 4
          and '-184735220' not in channels.values(), 'the call, share, reflect and push channels are four different ones')
    body_max = re.search(r'SHARE_BODY_MAX\s*=\s*(\d+);', proto_h)
    payload_max = re.search(r'SHARE_PAYLOAD_MAX\s*=\s*(\d+);', proto_h)
    seq_digits = re.search(r'SHARE_SEQ_DIGITS_MAX\s*=\s*(\d+);', proto_h)
    check(body_max is not None and lsl_global('BODY_MAX') == body_max.group(1) and body_max.group(1) == '1100',
          'the script takes a body of at most proto::SHARE_BODY_MAX (1100) bytes')
    check(payload_max is not None and lsl_global('PAYLOAD_MAX') == payload_max.group(1) and payload_max.group(1) == '900',
          'a payload of at most proto::SHARE_PAYLOAD_MAX (900)')
    check(seq_digits is not None and lsl_global('SEQ_DIGITS_MAX') == seq_digits.group(1) and seq_digits.group(1) == '9',
          'a sequence number of at most 9 digits')
    check(lsl_global('SHARE_GAP') == '0.3' and lsl_global('SHOUT_MAX') == '6' and lsl_global('GAP_MAX') == '32',
          'one avatar\'s share or push at most every 0.3 s, at most 6 things said a second, 32 avatars kept')
    for tag in ('<charisShare>', '</charisShare>', '<charisReflect>', '</charisReflect>', '<charisPush>', '</charisPush>'):
        check(tag in lsl and tag in proto_cpp, 'tag %s in both' % tag)
    for body in ('share|', 'shareend|', 'reflect|', 'push|', 'pushend|'):
        check('body("%s")' % body in proto_cpp and ('verb == "%s"' % body[:-1]) in lsl, 'the %s body of the viewer and the script' % body[:-1])
    check('"share1|' in lsl and '"push1|' in lsl and '"reflect1|' in lsl, 'the messages of the three kinds the script says and hears')
    heard = re.search(r'\nrelayPayload\(key thing, string text, integer isShare\)\n\{\n(.*?)\n\}\n', lsl, re.S)
    check(heard is not None and 'llParseString' not in heard.group(1) and 'list' not in re.sub(r'//[^\n]*', '', heard.group(1)),
          'a share or push heard is cut with llSubStringIndex, never split into a list (it comes from anyone near, before '
          'anything is known of the speaker: a line of bars would make a list of 950 items)')
    check('llResetTime();' in lsl.split('startBridge()', 1)[1].split('stopBridge()', 1)[0],
          'startBridge() restarts llGetTime() (a float: its steps stay finer than the 0.3 s gap)')
    check(re.search(r'SHARE_LINE_MAX\s*=\s*1000;', proto_h) is not None and re.search(r'PUSH_LINE_MAX\s*=\s*1000;', proto_h) is not None,
          'a told share or push line is at most 1000 bytes (llOwnerSay says 1024)')
    check(lsl_global('ASKERS_MAX') == '64' and lsl_global('ASK_GAP') == '5', '64 askers kept, 5 seconds between requests')
    actions_lsl = re.findall(r'"(\w+)"', lsl_global('ACTIONS') or '')
    actions_cpp = re.findall(r'"(\w+)"', re.search(r'ACTION_NAMES\[ACTION_COUNT\]\s*=\s*\{([^}]*)\}', proto_cpp).group(1))
    check(actions_lsl == actions_cpp == ['request', 'accept', 'refuse', 'cancel', 'withdraw'], 'the five actions in one order')
    for tag in ('<auroraPose>', '</auroraPose>', '<charisBridge>', '</charisBridge>', '<charisBridgeNoUrl>', '</charisBridgeNoUrl>'):
        check(tag in lsl and tag in proto_cpp, 'tag %s in both' % tag)
    check('body("hello|")' in proto_cpp and 'body("pose|")' in proto_cpp, 'the hello and pose bodies of the viewer')
    check('REPLY_OK[] = "ok"' in proto_h, 'the "ok" reply')
    gap = float(lsl_global('ANNOUNCE_GAP') or 0)
    answer = cpp_constant(bridge_cpp, 'constexpr F64 ANSWER_SECONDS')
    start_answer = cpp_constant(bridge_cpp, 'constexpr F64 START_ANSWER_SECONDS')
    check(answer is not None and answer >= gap + URL_GRANT_MARGIN,
          'the viewer waits for a new URL (ANSWER_SECONDS %s) at least ANNOUNCE_GAP (%s) + %s s for the URL to be '
          'granted: a missed first announcement is caught by the next' % (answer, gap, URL_GRANT_MARGIN))
    check(start_answer is not None and start_answer >= gap + URL_GRANT_MARGIN, 'START_ANSWER_SECONDS >= ANNOUNCE_GAP + margin')

    # The viewer says hello only through helloWhenClean(), which waits until the worn object's contents were seen
    # holding no other script for the URL held (a script put in the bridge could have said the URL itself).
    body = re.search(r'\n    void helloWhenClean\(\)\n    \{\n(.*?)\n    \}\n', bridge_cpp, re.S)
    calls = [m.start() for m in re.finditer(r'\bsendHello\(\);', bridge_cpp)]
    check(body is not None and 'b.cleanOf' in body.group(1), 'helloWhenClean() looks at the contents seen clean')
    check(body is not None and calls and all(body.start(1) <= c < body.end(1) for c in calls),
          'sendHello() is called from helloWhenClean() only (%d call(s))' % len(calls))
    extras = re.search(r'if \(!seen\.extraScripts\.empty\(\) && worn\)\n(.*?)\n        else if \(seen\.known && worn && !older\)\n'
                       r'        \{\n            b\.cleanOf = object_id;', bridge_cpp, re.S)
    check(extras is not None and 'b.cleanOf.setNull();' in extras.group(1) and len(re.findall(r'b\.cleanOf = ', bridge_cpp)) == 1,
          'the contents set cleanOf only when no other script was found (else if) and they answer our own request '
          '(not an older one on the way), and clear it when one was found')
    check(extras is not None and re.search(r'if \(!b\.url\.empty\(\) && !b\.helloOk\)\n\s*\{\n\s*dropUrl\(\);', extras.group(1)),
          'another script found in the bridge: the URL held (it may have said it) is dropped before any hello')

    def cpp_function(head):
        m = re.search(r'\n    ' + re.escape(head) + r'\n    \{\n(.*?)\n    \}\n', bridge_cpp, re.S)
        return m.group(1) if m else ''

    check('root != chatter ||' in cpp_function('bool isOurBridge(LLViewerObject* chatter)'),
          'a line of a child prim is not the bridge\'s: the chatter is the root of our attachment')
    ask = cpp_function('void askContents(LLViewerObject* object, ContentsAsk ask = ContentsAsk::First)')
    check(re.search(r'registerInventoryListener\(.*?\);.*?dirtyInventory\(\);.*?'
                    r'b\.contentsOlder = object->isInventoryPending\(\);\s*\n\s*object->requestInventory\(\);', ask, re.S),
          'the contents are asked of the simulator (the listener, then dirtyInventory(), then requestInventory()), '
          'not taken from the copy kept, and a request already on the way is noted (its answer is an older one)')
    contents = cpp_function('void onContents(const LLUUID& object_id, U32 ask_number, const ContentsSeen& seen)')
    older_block = re.search(r'if \(worn && older && b\.contentsAsk == ContentsAsk::First\)\n\s*\{\n(.*?)\n        \}\n',
                            contents, re.S)
    check(older_block is not None and 'askContents(object, ContentsAsk::Again);' in older_block.group(1),
          'an answer to an older request is asked once more (First only: not again and again)')
    check(older_block is not None and re.search(r'if \(b\.step == Step::CheckContents\)\n\s*\{\n\s*'
                                                r'b\.stepDeadline = now\(\) \+ STEP_SECONDS;', older_block.group(1)),
          'the one more look in CheckContents gets its own time (the deadline moved once)')
    # Each ask has its number; an answer left for the idle is taken only for the ask it answered.
    check('++b.contentsAskNumber;' in ask and 'const U32          ask_number = bridge().contentsAskNumber;' in bridge_cpp
          and 'ask_number != b.contentsAskNumber' in contents,
          'an answer waiting for the idle after a newer ask is dropped (the ask\'s number)')
    check('if (b.step == Step::AwaitAnswer && b.cleanOf == object_id)' in contents,
          'the contents say hello only when they set cleanOf (else the next announcement asks again: no ask loop)')
    # cleanOf is for the URL held: cleared by another URL and by a new attempt.
    check(re.search(r'if \(announcement\.url != b\.url\)\n\s*\{\n\s*b\.cleanOf\.setNull\(\);',
                    cpp_function('void onAnnouncement(const proto::Announcement& announcement)')),
          'an announcement of another URL clears cleanOf (its contents are looked at again)')
    check('b.cleanOf.setNull();' in cpp_function('void beginAttempt()'), 'a new attempt clears cleanOf')
    # The pose lines: another object's go on as any line; the bridge's are taken only once its contents were seen
    # clean (the hello came after that), and dropped unread (never shown) when they do not pass.
    pose = re.search(r'if \(kind == proto::LineKind::Pose\)\n        \{\n(.*?)\n        \}\n', bridge_cpp, re.S)
    pose = pose.group(1) if pose else ''
    unseen = re.search(r'if \(!from_bridge\)\n            \{\n(.*?)\n            \}\n', pose, re.S)
    unseen = unseen.group(1) if unseen else ''
    check(re.search(r'if \(from_id\.notNull\(\) && from_id == bridge\(\)\.cleanOf\)\n\s*\{\n[^}]*return true;', unseen),
          'a pose line of the bridge known by its id (cleanOf), its object not seen or not confirmed as the worn '
          'bridge, is dropped unread')
    check(re.search(r'\}\n\s*return false;[^\n]*$', unseen),
          'a pose line of an object that is not the bridge goes on as any line')
    # Each check of poseLineRefusal() with the word it gives, in this order; null only at the end (taken).
    refusal = cpp_function('const char* poseLineRefusal(const LLViewerObject* chatter, const std::string& text, '
                           'proto::PoseLine& line)')
    check(re.fullmatch(r'\s*if \(bridge\(\)\.cleanOf != chatter->getID\(\)\)\n\s*\{\n\s*return "contents";\n\s*\}\n'
                       r'\s*if \(!proto::parsePoseLine\(text, line\)\)\n\s*\{\n\s*return "form";\n\s*\}\n'
                       r'\s*if \(LLUUID\(line\.peer\) == gAgentID\)\n\s*\{\n\s*return "self";\n\s*\}\n'
                       r'\s*return nullptr;', refusal) is not None and refusal.count('return nullptr;') == 1,
          'a pose line of the bridge is refused without cleanOf ("contents"), out of the form ("form") and naming '
          'ourselves ("self"), and taken (null) only after all three')
    check(re.search(r'if \(const char\* refusal = poseLineRefusal\(chatter, text, line\)\)\n\s*\{\n[^}]*return true;', pose),
          'a pose line of the bridge that does not pass is dropped unread (not shown)')
    check(re.search(r'if \(b\.state != State::Off && b\.poseHandler\)\n\s*\{\n\s*b\.poseHandler\(', pose),
          'a pose line is passed on only while the bridge is not Off')
    # Which speaker is the bridge: fully audible, the worn bridge's root, the bridge item.
    check('const bool from_bridge = chatter && CHAT_AUDIBLE_FULLY == chat.mAudible && isOurBridge(chatter);' in bridge_cpp,
          'a line is the bridge\'s only when fully audible from the worn bridge')
    check('            return worn_item == b.item;\n' in cpp_function('bool isOurBridge(LLViewerObject* chatter)'),
          'a worn object is the bridge only when it is the bridge item (once that is known)')
    check(re.search(r'if \(!from_bridge\)\n\s*\{\n\s*LL_INFOS\("PlusLSLBridge"\) << "A bridge line of an object that is not '
                    r'the worn bridge: dropped unread" << LL_ENDL;\n\s*return true;', bridge_cpp),
          'a line in the form of the bridge\'s URL lines from any other object is dropped unread (not shown)')
    # A look at the contents of an object no longer worn is not used to add the script.
    check(re.search(r'if \(b\.step == Step::CheckContents\)\n\s*\{\n\s*if \(!worn\)\n\s*\{\n(?:\s*//[^\n]*\n)*'
                    r'\s*if \(object\)\n\s*\{\n\s*askContents\(object\);\n\s*\}\n\s*return;', contents),
          'in CheckContents an answer for an object not worn now adds no script (the worn one is looked at)')
    # What that leans on: the answer is the worn object's only by its id; a take-off in CheckContents is watched and
    # is a repair (the hello is waited for there).
    check('        const bool      worn   = object && object->getID() == object_id;\n' in contents,
          'the contents are the worn object\'s only when its id is the one asked of')
    check(re.search(r'const bool watched = \(b\.state == State::Running \|\| b\.state == State::Silent \|\|\n\s*'
                    r'b\.step == Step::CheckContents \|\| b\.step == Step::AddScript \|\| b\.step == Step::AwaitAnswer\);',
                    cpp_function('void watchWorn()')),
          'watchWorn() watches Running, Silent, CheckContents, AddScript and AwaitAnswer')
    check(re.search(r'const bool hello_waited = b\.step == Step::CheckContents \|\|\n\s*'
                    r'\(b\.step == Step::AwaitAnswer && !b\.url\.empty\(\) && !b\.helloOk\);', cpp_function('void onTakenOff()')),
          'a take-off in CheckContents, or in AwaitAnswer with a URL and no hello answered, is a repair')
    # The gates of what is taken and sent.
    check(re.search(r'if \(CHAT_TYPE_OWNER != chat\.mChatType \|\| CHAT_SOURCE_OBJECT != chat\.mSourceType \|\| '
                    r'owner_id != gAgentID\)\n\s*\{\n\s*return false;', bridge_cpp),
          'only the llOwnerSay lines of our own objects are looked at')
    our = cpp_function('bool isOurBridge(LLViewerObject* chatter)')
    check('if (worn_item != recordedItem())' in our and 'bridgeName()' not in our and 'CHARIS_FOLDER_NAME' not in our,
          'before the item is known, a worn object is the bridge only by its record (never by its name or place)')
    check('options->setFollowRedirects(false);' in bridge_cpp, 'the posts follow no redirect')
    check(re.search(r'if \(b\.state != State::Running \|\| !b\.helloOk \|\| b\.url\.empty\(\) \|\|',
                    cpp_function('bool sendRequest(PostKind kind, PoseAction action, const LLUUID& nonce, const LLUUID& target)')),
          'a pose (or a reflect message) is sent only while Running, after the hello, with a URL')
    check("if (!seen.ours && it->getName() == bridgeName() && it->getCreatorUUID() == gAgentID)" in bridge_cpp,
          'our script in the bridge is known by its name and by being made by us')
    # The script item is counted only when it went before it was put in (made again), not at every making.
    uploaded = cpp_function('void onScriptUploaded(U32 epoch, const LLUUID& script_item, const LLSD& response)')
    check(bridge_cpp.count('++bridge().scriptTries') + bridge_cpp.count('++b.scriptTries') == 1
          and re.search(r'if \(!script\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*if \(\+\+bridge\(\)\.scriptTries > MAX_SCRIPT_TRIES\)',
                        uploaded) and 'scriptTries' not in cpp_function('void addScript()'),
          'the script tries are counted only when the script item went before it was put in')
    check(re.search(r'removeOtherBridgeItems\(\);\n\s*setStep\(Step::AddScript, SCRIPT_SECONDS\);', cpp_function('void addScript()')),
          'the script items of earlier makings go to the trash before a new one is made')
    # Every making counts toward MAX_SCRIPT_MAKES (5); an upload that finds the bridge off is a take-off (its turns).
    check(re.search(r'constexpr int MAX_SCRIPT_MAKES\s*= 5;', bridge_cpp)
          and re.search(r'if \(\+\+b\.scriptMakes > MAX_SCRIPT_MAKES\)\n\s*\{\n\s*fail\(Failure::Make, ', cpp_function('void addScript()')),
          'script items are made at most MAX_SCRIPT_MAKES (5) times an attempt, those cut short too, then Failed')
    check(re.search(r'if \(!object\)\n\s*\{\n\s*onTakenOff\(\);', uploaded),
          'an upload that finds the bridge taken off is a take-off (Detached and its turns), not a free wear')
    # The functions that touch the user's inventory, the bridge's checks and the limits: pinned as they are (comments
    # and spaces aside), so that a changed condition or answer is caught.
    def code(text):
        return ' '.join(re.sub(r'//[^\n]*', '', text).split())

    for head, body, what in PINNED:
        check(code(cpp_function(head)) == code(body), what)
    sharing = re.search(r'\n        bool handleSharingLine\(.*?\)\n        \{\n(.*?)\n        \}\n    \}\n', bridge_cpp, re.S)
    check(sharing is not None and code(sharing.group(1)) == code(SHARING_LINE_BODY),
          'a sharing line is taken only from the worn bridge with its contents seen clean (cleanOf), in its form, not naming '
          'ourselves and not while Off; dropped unread when it does not pass, and always dropped when the speaker is not the worn bridge')
    check(re.search(r'if \(kind == proto::LineKind::Share \|\| kind == proto::LineKind::Push \|\| kind == proto::LineKind::Reflect\)\n'
                    r'\s*\{\n\s*return handleSharingLine\(kind, text, chatter, from_bridge\);', bridge_cpp),
          'handleOwnerSay() gives the sharing kinds to handleSharingLine() (before the bridge\'s own URL lines)')
    check(re.search(r'if \(kind != PostKind::Share && kind != PostKind::Push\)\n\s*\{\n\s*\+\+b\.postsInFlight;', cpp_function(
          'void post(const std::string& body, PostKind kind, const PoseSent& sent = PoseSent())'))
          and 'if (!laneKind && b.postsInFlight > 0)' in bridge_cpp,
          'the sharing posts are not counted in the 16 posts of sendPose (a lane of their own)')
    for const, value in (('MAX_LANE_POSTS', '8'), ('MAX_SHARES_PER_WINDOW', '4'), ('SHARE_WINDOW_SECONDS', '1.0'), ('MAX_POSTS_IN_FLIGHT', '16')):
        check(re.search(const + r'\s*=\s*' + re.escape(value) + ';', bridge_cpp), '%s is %s' % (const, value))
    applied = cpp_function('void applyPostResult(PostKind kind, PostResult result)')
    check(re.search(r'if \(result == PostResult::Rate\)\n\s*\{\n\s*if \(lane\)\n\s*\{\n\s*laneAnswered\(\);\n\s*LL_DEBUGS\("PlusLSLBridge"\)[^\n]*\n\s*return;\n\s*\}\n'
                    r'\s*result = PostResult::Refused;', applied),
          'the script\'s rate refusal (429) of a sharing post is a debug line; any other post told 429 takes it as refused')
    check(re.search(r'if \(lane && result == PostResult::Transient\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*laneFailed\(\);\n\s*return;', applied),
          'a sharing post that failed for a busy simulator or a timeout is not counted toward MAX_TRANSIENT_FAILURES (only the '
          'hello and the pose messages are): laneFailed() judges them by time')
    check(re.search(r'b\.transientFailures = 0;\n\s*laneAnswered\(\);\n\s*if \(kind == PostKind::Hello && !b\.helloOk\)\n\s*\{\n\s*b\.laneRefusalTold = false;',
                    applied), 'an ok answer ends the run of failed sharing posts; a new URL\'s hello renews the once-only refusal warning')
    check(re.search(r'if \(result == PostResult::Refused\)\n\s*\{\n\s*if \(lane\)\n\s*\{\n\s*laneAnswered\(\);\n\s*\}\n\s*if \(lane && b\.laneRefusalTold\)', applied),
          'a refusal of a sharing post is an answer too: the run of failures ends')
    check(re.search(r'if \(lane && b\.laneRefusalTold\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*LL_DEBUGS\("PlusLSLBridge"\) << "Bridge refused a "[^\n]*\n\s*\}\n'
                    r'\s*else\n\s*\{\n\s*b\.laneRefusalTold = b\.laneRefusalTold \|\| lane;\n\s*LL_WARNS\("PlusLSLBridge"\) << "Bridge refused a "', applied),
          'a refused sharing post (not the rate) is warned once, then a debug line')
    check(re.search(r'\n        urlLost\(\);\n    \}\n$', applied) is not None or applied.rstrip().endswith('urlLost();'),
          'the transient failures of hello and pose end in urlLost() as before')
    check(re.search(r'if \(code == 429\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*return PostResult::Rate;', cpp_function('PostResult classify(const LLSD& result)')),
          '429 is the script\'s rate refusal (PostResult::Rate), not any refusal')
    check('SHARE_SPACING_SECONDS = 0.35;' in bridge_cpp and 'shareLaunches.clear();' in cpp_function('void dropUrl()') and 'endLaunches.clear();' in cpp_function('void dropUrl()')
          and 'laneFailSince = 0.0;' in cpp_function('void dropUrl()') and 'laneFailLast  = 0.0;' in cpp_function('void dropUrl()')
          and 'LANE_FAIL_SECONDS     = 15.0;' in bridge_cpp and 'LANE_FAIL_GAP_SECONDS = 12.0;' in bridge_cpp
          and 'MAX_ENDS_PER_WINDOW   = 2;' in bridge_cpp,
          'a key\'s sharing posts are 0.35 s apart, counted from the end of the last one (the receiving script takes 0.3 s); '
          'dropUrl() restarts the window of launches and the run of failures; sharing posts failing 15 s lose the URL')
    check(re.search(r'onLanePostDone\(kind, sent\.key, epoch\);', bridge_cpp) and bridge_cpp.count('shareLane.clear();') == 1
          and bridge_cpp.count('pushLane.clear();') == 1, 'a sharing post frees its key after the result is applied; dropUrl() clears the lanes')
    check(bridge_cpp.count('gInventory.removeItem(') == 1 and 'purgeObject' not in bridge_cpp
          and bridge_cpp.count('removeOwnItem(') == 3,
          'items go to the trash only through removeOwnItem() (from removeOtherBridgeItems() and forgetItem()), never purged')
    check('if (used > 0 && ranSince && now() - lastAt >= FORGIVE_SECONDS)' in bridge_cpp
          and '            ++used;\n            lastAt   = now();\n            ranSince = false;\n            return true;' in bridge_cpp
          and re.search(r'if \(state == State::Running\)\n\s*\{\n\s*b\.repairs\.ranSince = true;\n\s*b\.retries\.ranSince = true;',
                        bridge_cpp),
          'a count is forgiven only after Running and FORGIVE_SECONDS since its last try')
    check(re.search(r'if \(!b\.repairs\.take\(MAX_REPAIRS\)\)\n\s*\{\n\s*fail\(Failure::GaveUp, why\);',
                    cpp_function('void repair(Repair kind, const char* why)')),
          'a repair takes one of MAX_REPAIRS, then Failed (GaveUp)')
    check('if (!b.retries.take(MAX_RETRIES))' in cpp_function('void scheduleRetry()'), 'a retry takes one of MAX_RETRIES')
    check(re.search(r'if \(bridge\(\)\.state == State::Off\)\n\s*\{\n\s*start\(\);\n\s*return;\n\s*\}\n\s*wake\(\);',
                    cpp_function('void ensure()')),
          'ensure() starts only from Off (else it only wakes the tick)')
    attempt = cpp_function('void beginAttempt()')
    check('b.repairs.reset();' in attempt and 'b.scriptTries  = 0;' in attempt and 'b.scriptMakes  = 0;' in attempt,
          'a new attempt counts its repairs, script tries and makings from 0')
    # A wear asked for and coming after the bridge stopped is taken off again.
    check(re.search(r'addAttachmentRequest\(b\.item, ATTACH_POINT, true\);\n\s*b\.wearAskedOf = b\.item;', bridge_cpp)
          and 'LLVOAvatarSelf::detachAttachmentIntoInventory(b.wearAskedOf);' in cpp_function('bool watchLateWear()')
          and 'const bool stopped = b.state == State::Off || (b.state == State::Failed && b.retryAt <= 0.0);'
          in cpp_function('bool watchLateWear()')
          and bridge_cpp.count('watchLateWear();') == 1
          and re.search(r'if \(b\.wearAskedOf\.notNull\(\)\)\n\s*\{\n\s*wake\(\);', cpp_function('void stop()')),
          'a wear asked for that comes after the bridge stopped (Off, Failed with no new try) is taken off again')
    # The note of a wear on its way goes whenever the bridge is seen worn (not only in Wear), and a new attempt and
    # the hello start it and the makings from 0.
    hello_ok = re.search(r'if \(kind == PostKind::Hello && !b\.helloOk\)\n\s*\{\n(.*?)\n            \}\n', bridge_cpp, re.S)
    hello_ok = hello_ok.group(1) if hello_ok else ''
    check('b.wearAskedOf.setNull();' in hello_ok and 'b.scriptMakes  = 0;' in hello_ok,
          'the hello answered ends the note of a wear on its way and starts the makings from 0 (a long session)')
    check(re.search(r'if \(wornObject\(\)\)\n\s*\{\n\s*b\.notWornSince = 0\.0;\n\s*b\.wearAskedOf\.setNull\(\);',
                    cpp_function('void watchWorn()'))
          and re.search(r'return; // being taken off by us\n\s*\}\n\s*b\.wearAskedOf\.setNull\(\);',
                        cpp_function('void onAnnouncement(const proto::Announcement& announcement)'))
          and 'b.wearAskedOf.setNull();' in attempt,
          'a wear on its way is forgotten once the bridge is seen worn (watchWorn, its announcement) and at a new attempt')
    # The setting gates every way in.
    check(cpp_function('void onAnnouncement(const proto::Announcement& announcement)').lstrip().startswith(
              'Bridge& b = bridge();\n        if (!settingOn())\n        {\n            takeOffWhileOff();\n            return;\n        }')
          and cpp_function('void onNoUrl()').lstrip().startswith(
              'Bridge& b = bridge();\n        if (!settingOn())\n        {\n            takeOffWhileOff();\n            return;\n        }')
          and 'if (!LLApp::isRunning() || !settingOn())\n        {\n            return;\n        }' in cpp_function('void ensure()')
          and re.search(r'if \(settingOn\(\)\)\n\s*\{\n\s*start\(\);', cpp_function('void onUIReady()')),
          'with the setting off nothing starts (an announcement or a no-URL line takes the bridge off), ensure() and '
          'the login do nothing')
    check(re.search(r'if \(b\.state == State::Off\)\n\s*\{\n\s*const bool late_wear = watchLateWear\(\) \|\| gone_old;[^\n]*\n'
                    r'\s*b\.ticking\s*= late_wear;\n\s*return !late_wear;', cpp_function('bool tick()')),
          'while Off the tick runs only while a late wear is watched')
    check(re.search(r'if \(wornObject\(\)\)\n\s*\{\n\s*b\.ownDetach = OwnDetach::Off;\n\s*'
                    r'LLVOAvatarSelf::detachAttachmentIntoInventory\(b\.item\);', cpp_function('void stop()')),
          'the setting turned off takes the worn bridge off')
    check(re.search(r'leaveSteps\(\);\n\s*bridge\(\)\.ownDetach = OwnDetach::None;\n\s*setState\(State::Failed\);',
                    cpp_function('void fail(Failure failure, const char* why)')),
          'Failed forgets a take-off of ours in progress')
    # Which object is the bridge item, what is taken off, what is our script, what is copied.
    find_item = cpp_function('void stepFindItem()')
    check('const LLUUID                 recorded = recordedItem();' in find_item
          and re.search(r'const LLUUID found = \(it && !it->getIsLinkType\(\) && it->getType\(\) == LLAssetType::AT_OBJECT && '
                        r'!inTrash\(recorded\)\)\n\s*\? recorded\n\s*: LLUUID::null;', find_item)
          and 'bridgeName()' not in find_item,
          'the bridge item is only the object recorded when this viewer made it (not one found by its name), not a link, '
          'not in the trash')
    # The record: written when the library object is copied, cleared when that object is made anew, read checked.
    check(re.search(r'b\.item = new_item;\n\s*recordItem\(new_item\);', cpp_function('void onItemCopied(U32 epoch, const LLUUID& new_item)')),
          'the copied bridge object is recorded for the account')
    check('static LLCachedControl<bool> enabled(gSavedPerAccountSettings, SETTING_ENABLE, true); // as settings_plus.xml'
          in bridge_cpp
          and 'gSavedPerAccountSettings.getControl(SETTING_ENABLE)' in bridge_cpp
          and 'gSavedSettings.getControl(SETTING_ENABLE)' not in bridge_cpp,
          'the setting is the account\'s (gSavedPerAccountSettings), on by default (research/353)')
    # research/353: on by default. A safe start runs nothing (D5), the failures are shown only when the person turned
    # the bridge on in this session (section 3), and the bridge is made once the login's outfit is on (D4).
    check(cpp_function('bool settingOn()').rstrip().endswith('return enabled && !plus::safemode::active();'),
          'a safe start leaves the bridge off (every way in reads settingOn())')
    changed = cpp_function('void onSettingChanged(bool on)')
    check(re.search(r'if \(!on\)\n\s*\{\n\s*stop\(\);\n\s*return;\n\s*\}\n', changed)
          and re.search(r'if \(LLStartUp::getStartupState\(\) == STATE_STARTED\)\n\s*\{\n\s*b\.turnedOnByUser = true;', changed)
          and changed.count('b.turnedOnByUser = true;') == 1 and bridge_cpp.count('turnedOnByUser = true') == 1
          and re.search(r'if \(plus::safemode::active\(\)\)\n\s*\{\n[^\n]*\n\s*return;\n\s*\}\n\s*start\(\);$', changed.rstrip()),
          'the setting turned on: the person\'s only once logged in, and no start in a safe start')
    check(re.search(r'b\.notified \|= bit;\n\s*if \(!b\.turnedOnByUser\)\n\s*\{\n\s*LL_INFOS[^\n]*\n\s*return;\n\s*\}\n\s*switch \(failure\)',
                    cpp_function('void notifyFailure(Failure failure)')),
          'a failure is shown only when the person turned the bridge on in this session (else logged once a kind)')
    check(cpp_function('bool outfitBeingPutOn()').strip() == 'return gAgentWearables.changeInProgress();'
          and re.search(r'gInventory\.getRootFolderID\(\)\.isNull\(\) \|\| outfitBeingPutOn\(\)\)\n\s*\{\n\s*return;',
                        cpp_function('void stepWaitLogin()')),
          'the bridge waits for the login\'s outfit (its end takes off what it did not list)')
    check(re.search(r'if \(!b\.wearAsked\)\n\s*\{\n\s*if \(outfitBeingPutOn\(\)\)\n\s*\{\n\s*b\.stepDeadline = now\(\) \+ WEAR_SECONDS;'
                    r'[^\n]*\n\s*return;\n\s*\}\n', cpp_function('void stepWear()')),
          'a wear is asked for only once an outfit being put on is on, with the step\'s whole time')
    # Linked parts: the bridge is one prim; one with others linked is not used (at its wear, its contents, its hello).
    check(re.search(r'b\.wearAskedOf\.setNull\(\);[^\n]*\n\s*if \(!object->isHUDAttachment\(\)\)\n\s*\{\n(?:\s*//[^\n]*\n)*'
                    r'\s*repair\(Repair::Rewear, "worn on the body"\);\n\s*return;\n\s*\}\n\s*if \(refuseLinkedParts\(object\)\)',
                    cpp_function('void stepWear()'))
          and 'if (worn && refuseLinkedParts(object))' in contents
          and 'if (refuseLinkedParts(object))' in cpp_function('void helloWhenClean()'),
          'a bridge with other prims linked to it is refused at its wear, its contents and before its hello')
    # The posts check the certificate and its host; a certificate that does not pass is Failed at once (not repaired).
    check('options->setSSLVerifyPeer(true);' in bridge_cpp and 'options->setSSLVerifyHost(true);' in bridge_cpp,
          'the posts check the certificate and that it names the host')
    check('constexpr short CURL_PEER_NOT_VERIFIED_OLD = 51;' in bridge_cpp and 'constexpr short CURL_PEER_NOT_VERIFIED     = 60;' in bridge_cpp
          and re.search(r'if \(result == PostResult::Certificate\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*if \(!b\.certificateTold\)\n\s*\{\n'
                        r'\s*b\.certificateTold = true;\n\s*LL_WARNS[^\n]*the certificate does not name the host[^\n]*\n\s*\}\n'
                        r'\s*fail\(Failure::Make, "certificate"\);\n\s*b\.certificateFailed = true;[^\n]*\n\s*return;',
                        cpp_function('void applyPostResult(PostKind kind, PostResult result)')),
          'a certificate that does not name the host (curl 51, 60) is told once and Failed (Make), not made anew, and marked')
    # After that, the same URL said again is not taken before the retry's time (or a region change): the retries keep
    # their gaps.
    check(re.search(r'if \(b\.state == State::Failed && b\.certificateFailed\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*return;',
                    cpp_function('void onAnnouncement(const proto::Announcement& announcement)'))
          and 'b.certificateFailed = false;' in attempt
          and 'b.certificateFailed = false;' in cpp_function('void onRegionChanged()'),
          'after a certificate failure the announcements wait for the retry (beginAttempt) or a region change')
    # The record is saved at once (a crash must not lose it), the recorded item not yet read is asked for and waited.
    check(re.search(r'setString\(SETTING_ITEM, [^\n]*\n(?:\s*//[^\n]*\n)*\s*if \(LLStartUp::getStartupState\(\) == STATE_STARTED\)\n\s*\{\n'
                    r'\s*plus::saveSettingsFile\(gSavedPerAccountSettings, gSavedSettings\.getString\("PerAccountSettingsFile"\), true\);',
                    cpp_function('void recordItem(const LLUUID& id)')),
          'the record is saved to the account\'s file at once (in-world)')
    check(re.search(r'if \(recorded\.notNull\(\) && !it && !stepExpired\(\)\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*if \(!b\.itemFetchAsked\)\n'
                    r'\s*\{\n\s*b\.itemFetchAsked = true;\n\s*LLInventoryModelBackgroundFetch::instance\(\)\.scheduleItemFetch\(recorded, true\);\n'
                    r'\s*\}\n\s*return;', find_item),
          'a recorded bridge not read yet is asked for by its id and waited for, not made a second time at once')
    # The settings: the switch and the record per account, the record hidden from the Debug Settings.
    settings = read(SETTINGS_XML)

    def setting_entry(name):
        m = re.search(r'<key>' + name + r'</key>\s*<map>(.*?)</map>', settings, re.S)
        return m.group(1) if m else ''

    enable, item = setting_entry('PlusPoseBridgeEnable'), setting_entry('PlusPoseBridgeItem')
    check(re.search(r'<key>PerAccount</key>\s*<integer>1</integer>', enable) and 'HideFromEditor' not in enable
          and re.search(r'<key>PerAccount</key>\s*<integer>1</integer>', item)
          and re.search(r'<key>HideFromEditor</key>\s*<integer>1</integer>', item),
          'PlusPoseBridgeEnable and PlusPoseBridgeItem are per account; the record is hidden from the Debug Settings')
    # On by default (research/353), the cached control's own default the same.
    check(re.search(r'<key>Value</key>\s*<integer>1</integer>', enable)
          and 'SETTING_ENABLE, true); // as settings_plus.xml' in bridge_cpp,
          'PlusPoseBridgeEnable is on by default in settings_plus.xml, as the cached control\'s default')
    # ---- research/353, the pre-merge review
    # H1: the bridge is no part of an outfit (llappearancemgr.cpp, as Firestorm's): not a change of it, not saved into
    # it, kept on through a replace while it is to run. Only the recorded object counts.
    appearance = read(APPEARANCE_CPP)

    def official_function(text, head):
        m = re.search(r'\n' + re.escape(head) + r'[^{;]*?\n\{\n(.*?)\n\}\n', text, re.S)
        return m.group(1) if m else ''

    check('#include "plus/lslbridge/lslbridgehooks.h" // <plus> lslbridge' in appearance
          and re.search(r'const LLViewerInventoryItem\* item = \(\*iter\);\n\s*// <plus> lslbridge[^\n]*\n'
                        r'\s*if \(plus::lslbridge::isBridgeItem\(item->getLinkedUUID\(\)\)\)\n\s*\{\n\s*continue;\n\s*\}\n'
                        r'\s*// </plus>\n\s*switch \(item->getActualType\(\)\)',
                        official_function(appearance, 'void LLAppearanceMgr::slamCategoryLinks(const LLUUID& src_id, const LLUUID& dst_id,')),
          'saving an outfit (Save, Save As: slamCategoryLinks) leaves the bridge out')
    check(re.search(r'// <plus> lslbridge[^\n]*\n\s*plus::lslbridge::placeBridgeInNewCof\(obj_items, append\);\n'
                    r'\s*// </plus>\n\s*removeDuplicateItems\(obj_items\);',
                    official_function(appearance, 'void LLAppearanceMgr::updateCOF(const LLUUID& category, bool append)')),
          'a replace and an add both leave the bridge in the new COF once or not at all (before the duplicates go)')
    # The second review L2: the changed compare has one exclusion of the bridge (no Firestorm block), on both lists.
    check(appearance.count('plus::lslbridge::dropBridgeLinks(') == 2 and 'FSLSLBridge' not in appearance
          and appearance.count('plus::lslbridge::') == 4,
          'llappearancemgr.cpp holds exactly the four calls of research/353 (the compare\'s two, the save, the new COF)')
    check(re.search(r'// <plus> lslbridge[^\n]*\n\s*plus::lslbridge::dropBridgeLinks\(cof_items\);\n'
                    r'\s*plus::lslbridge::dropBridgeLinks\(outfit_items\);\n\s*// </plus>\n\n\s*if\(outfit_items\.size\(\) != cof_items\.size\(\)\)',
                    official_function(appearance, 'void LLAppearanceMgr::updateIsDirty()')),
          'the worn bridge is no change of the outfit (the COF and the outfit compared without it)')
    check(cpp_function('bool isBridgeItem(const LLUUID& item_id)').strip() == 'return item_id.notNull() && item_id == recordedItem();'
          and re.search(r'const LLUUID ours = recordedItem\(\);.*\{ return item\.notNull\(\) && item->getLinkedUUID\(\) == ours; \}',
                        cpp_function('void dropBridgeLinks(std::vector<LLPointer<LLViewerInventoryItem>>& items)'), re.S)
          and 'if (ours.isNull() || !settingOn() || !isAgentAvatarValid())' in cpp_function('LLViewerInventoryItem* bridgeForOutfit(U32 other_objects)')
          and 'if (!gAgentAvatarp->getWornAttachment(ours) && !LLAppearanceMgr::instance().isLinkedInCOF(ours))'
              in cpp_function('LLViewerInventoryItem* bridgeForOutfit(U32 other_objects)'),
          'the outfit\'s hooks know the bridge only as the recorded object, and keep it only while it is to run and worn (or on its way)')
    # The second review M: the person's attachments first. The new COF's others (each once), the temporary ones, the
    # bridge and one place left must fit, else the bridge is left out of it (and comes off); a limit not known fits.
    place = cpp_function('void placeBridgeInNewCof(std::vector<LLPointer<LLViewerInventoryItem>>& obj_items, bool append)')
    fits = cpp_function('bool bridgeFitsWith(U32 other_objects)')
    check(re.search(r'if \(ours\.isNull\(\) \|\| !settingOn\(\)\)\n\s*\{\n\s*return;', place)
          and 0 <= place.find('others.insert(item->getLinkedUUID());') < place.find('dropBridgeLinks(obj_items);')
              < place.find('bridgeForOutfit(static_cast<U32>(others.size()))')
          and 'if (item.notNull() && item->getLinkedUUID() != ours)' in place
          and 'if (!bridgeFitsWith(other_objects))' in cpp_function('LLViewerInventoryItem* bridgeForOutfit(U32 other_objects)')
          and 'return other_objects + countTempAttachments() + 2 <= static_cast<U32>(most);' in fits
          and re.search(r'if \(most < 0\)\n\s*\{\n\s*return true;', fits)
          # the third review L1: one count of the temporary attachments, for roomForBridge() (through bridgeFitsWith) too
          and 'temporary += (object.notNull() && !object->isDead() && object->isTempAttachment()) ? 1 : 0;'
              in cpp_function('U32 countTempAttachments()')
          and cpp_function('U32 countTempAttachments()').rstrip().endswith('return temporary;')
          and bridge_cpp.count('isTempAttachment()') == 1 and bridge_cpp.count('countTempAttachments()') == 2,
          'the bridge stays in a new COF only with a place, the person\'s attachments first (theirs, the temporary ones, one left)')
    # M1: no attachment place to spare (one left for the person): not asked for, then Failed, tried again only once a
    # change of the attachments leaves a place (the second review).
    check(re.search(r'enum class Failure \{ NoLibrary, Make, GaveUp, NotHere, NoSlot \};', bridge_cpp)
          and 'case Failure::NoSlot:    LLNotificationsUtil::add("PlusLSLBridgeNoSlot"); break;' in bridge_cpp
          and re.search(r'if \(failure == Failure::Make \|\| failure == Failure::GaveUp\)\n\s*\{\n\s*scheduleRetry\(\);',
                        cpp_function('void fail(Failure failure, const char* why)'))
          and all('name="PlusLSLBridgeNoSlot"' in read(NOTIFICATIONS_XML[lang]) for lang in ('en', 'ja'))
          and 'bridge().waitingForPlace = failure == Failure::NoSlot;' in cpp_function('void fail(Failure failure, const char* why)')
          and re.search(r'if \(b\.state == State::Failed && b\.waitingForPlace && roomForBridge\(\)\)\n\s*\{\n(?:\s*//[^\n]*\n)*'
                        r'\s*LL_INFOS[^\n]*\n\s*beginAttempt\(\);', cpp_function('bool tick()'))
          and 'b.waitingForPlace = false;' in cpp_function('void beginAttempt()') and 'b.waitingForPlace = false;' in cpp_function('void stop()'),
          'no attachment place is a Failed of its own (notification en, ja), not tried by the clock, tried again once a place is free')
    room = cpp_function('bool roomForBridge()')
    check('!gAgentAvatarp->canAttachMoreObjects(2)' in room and room.rstrip().endswith('return bridgeFitsWith(listed);')
          and 'listed += (link->getLinkedUUID() != item) ? 1 : 0;' in room
          and re.search(r'const bool ask = !b\.waitForCof \|\| !LLAppearanceMgr::instance\(\)\.isLinkedInCOF\(b\.item\);\n'
                        r'\s*if \(ask && !roomForBridge\(\)\)\n\s*\{\n(?:\s*//[^\n]*\n)*\s*if \(stepExpired\(\)\)\n\s*\{\n'
                        r'\s*fail\(Failure::NoSlot, [^\n]*\n\s*\}\n\s*return;', cpp_function('void stepWear()')),
          'the bridge is asked for only with a place to spare (worn and listed in the COF, one left), else Failed (NoSlot)')
    # L1: a bridge that started by itself is only put on again by Auto repairs (never made anew for not answering).
    check('(kind == Repair::Auto && b.repairs.used > 1 && b.turnedOnByUser);' in cpp_function('void repair(Repair kind, const char* why)'),
          'Auto repairs make the bridge anew only when the person turned it on in this session')
    # M2: Failed with no new try to come is told apart in the poser.
    check(cpp_function('bool failedForGood()').strip().endswith('return b.state == State::Failed && b.retryAt <= 0.0 && !b.waitingForPlace;')
          and 'return plus::lslbridge::failedForGood() ? ConsentBlock::BridgeFailed : ConsentBlock::NoBridge;' in read(POSER_CPP)
          and 'case ConsentBlock::BridgeFailed: text = "consent_block_bridge_failed"; tip = "consent_block_bridge_failed_tip"; break;'
              in read(POSER_CPP)
          and all('name="consent_block_bridge_failed"' in read(POSER_XML[lang]) and 'name="consent_block_bridge_failed_tip"'
                  in read(POSER_XML[lang]) for lang in ('en', 'ja')),
          'the poser tells a bridge that will not try again (turn it off and on) from one not connected yet')
    # The second review L1: the outfit backup keeps, compares and puts back the outfit without the bridge; a restore
    # keeps the worn bridge on by the same rule as updateCOF (bridgeForOutfit), outside what it compares.
    backup = read(BACKUP_CPP)
    check('#include "plus/lslbridge/lslbridgehooks.h"' in backup
          and 'if (item && item->getIsLinkType() && !plus::lslbridge::isBridgeItem(item->getLinkedUUID()))' in backup
          and '!plus::lslbridge::isBridgeItem(object->getAttachmentItemID())) // its placement is the bridge\'s own' in backup
          and re.search(r'for \(const Link& link : sSaved\.outfit\)\n\s*\{\n\s*if \(plus::lslbridge::isBridgeItem\(link\.id\)\)\n\s*\{\n\s*continue;', backup)
          and re.search(r'std::sort\(sent\.begin\(\), sent\.end\(\)\);\n(?:\s*//[^\n]*\n)*\s*if \(LLViewerInventoryItem\* bridge_item = '
                        r'plus::lslbridge::bridgeForOutfit\(objects\)\)\n\s*\{\n(?:[^\n]*\n){5}\s*contents\.append\(entry\);', backup),
          'the outfit backup leaves the bridge out of its copy, its compare and its placements, and keeps the worn one on as updateCOF')
    # A pose message accepted by sendPose() has its result told once.
    check(re.search(r'post\(body, kind, sent\);', cpp_function('bool sendRequest(PostKind kind, PoseAction action, const LLUUID& nonce, const LLUUID& target)'))
          and 'return sendRequest(PostKind::Pose, action, nonce, target);' in bridge_cpp
          and 'return sendRequest(PostKind::Reflect, action, nonce, target);' in bridge_cpp
          and '? proto::reflectBody(b.token.asString()' in bridge_cpp
          and 'onPostResult(epoch, kind, sent, classify(result));' in bridge_cpp,
          'a pose message posted carries what was sent to its result')
    check('if (!it || it->getType() != LLAssetType::AT_LSL_TEXT)' in bridge_cpp,
          'only the scripts in the worn bridge are judged (and removed)')
    check(re.search(r'it->getPermissions\(\)\.allowCopyBy\(gAgentID\) &&\n\s*\(it->getPermissions\(\)\.getMaskNextOwner\(\) & PERM_MODIFY\)',
                    bridge_cpp),
          'a library object is copied only when we may copy it and its next owner may change it')
    # The limits as numbers, the posts in flight, late results and the hello's answer.
    for line in ('constexpr int MAX_REPAIRS           = 3;', 'constexpr int MAX_RETRIES           = 4;',
                 'constexpr int MAX_REWEARS           = 3;', 'constexpr F64 REWEAR_WINDOW_SECONDS = 600.0;',
                 'constexpr F64 FORGIVE_SECONDS       = 600.0;', 'constexpr U32 MAX_POSTS_IN_FLIGHT   = 16;',
                 'constexpr F64 WEAR_SECONDS          = 60.0;'):
        check(line in bridge_cpp, 'lslbridge.cpp keeps its limit: %s' % line.split('constexpr ')[1])
    check('target == gAgentID || b.postsInFlight >= MAX_POSTS_IN_FLIGHT)' in bridge_cpp,
          'at most MAX_POSTS_IN_FLIGHT posts wait for their answer')
    check('.asString() == proto::REPLY_OK)' in cpp_function('PostResult classify(const LLSD& result)'),
          'a post is Ok only when the bridge answered "ok"')
    # The callbacks of the inventory, the folder and the upload drop a result of another attempt or URL (the epoch).
    for head, cond in (('void onScriptUploaded(U32 epoch, const LLUUID& script_item, const LLSD& response)',
                        'if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::AddScript)'),
                       ('void onScriptItemMade(U32 epoch, const LLUUID& script_item)',
                        'if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::AddScript)'),
                       ('void onItemCopied(U32 epoch, const LLUUID& new_item)',
                        'if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::CopyItem || new_item.isNull())'),
                       ('void onFolderMade(U32 epoch, const LLUUID& folder)',
                        'if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::MakeFolder)')):
        check(re.search(re.escape(cond) + r'\n\s*\{\n\s*return;', cpp_function(head)),
              '%s drops a late result (its epoch, its step)' % head.split('(')[0].split()[-1])
    check('if (LLApp::isRunning() && epoch == bridge().epoch && bridge().step == Step::AddScript)' in bridge_cpp,
          'a refused upload of another attempt is not taken')
    # Silent: repaired only where scripts run, not while the region gives no URL, after its answer was waited for.
    check(re.search(r'if \(b\.state == State::Silent && b\.step == Step::None && !b\.urlRefused\)\n\s*\{\n'
                    r'\s*if \(!scriptsRunHere\(\)\)\n\s*\{\n\s*b\.answerWaited = false;[^\n]*\n\s*\}\n'
                    r'\s*else if \(!b\.answerWaited\)\n\s*\{\n(?:\s*//[^\n]*\n)?[^\n]*\n\s*setStep\(Step::AwaitAnswer, START_ANSWER_SECONDS\);\n'
                    r'\s*\}\n\s*else\n\s*\{\n\s*repair\(Repair::Auto, "not answering"\);', cpp_function('bool tick()')),
          'a Silent bridge is repaired only where scripts run, with a URL, after one more wait there')
    check('if (announcement.version != proto::BRIDGE_VERSION)\n        {\n            repair(Repair::Recreate, "other script version");'
          in bridge_cpp, 'an announcement of another script version makes the bridge anew')
    check('if (content.empty() || content.size() > SCRIPT_FILE_MAX_BYTES)' in bridge_cpp
          and 'constexpr std::size_t SCRIPT_FILE_MAX_BYTES = 64 * 1024;' in bridge_cpp,
          'the script file is taken only when not empty and at most 64 KB')
    for line in ('constexpr int MAX_SCRIPT_TRIES      = 2;', 'constexpr int MAX_REFUSED_ANNOUNCEMENTS = 2;',
                 'constexpr F64 RETRY_FIRST_SECONDS   = 60.0;', 'constexpr U32 MAX_TRANSIENT_FAILURES = 3;',
                 'constexpr F64 DETACH_SETTLE_SECONDS = 3.0;', 'constexpr F64 STEP_SECONDS          = 30.0;',
                 'constexpr F64 SCRIPT_SECONDS        = 120.0;', 'constexpr U32 POST_TIMEOUT_SECONDS  = 10;'):
        check(line in bridge_cpp, 'lslbridge.cpp keeps its limit: %s' % line.split('constexpr ')[1])
    check('b.scriptTries  = 0;' in hello_ok, 'the hello answered starts the script tries from 0 too')

    # The call (research/366): the viewer calls every PROBE_GAP_SECONDS, MAX_PROBES times in a step, and the script
    # answers a call at most every PROBE_GAP; it waits PAIR_WAIT for the first one.
    probe_gap_cpp = cpp_constant(bridge_cpp, 'constexpr F64 PROBE_GAP_SECONDS')
    max_probes = cpp_constant(bridge_cpp, 'constexpr int MAX_PROBES')
    pair_wait = float(lsl_global('PAIR_WAIT') or 0)
    check(probe_gap_cpp is not None and probe_gap_cpp > float(lsl_global('PROBE_GAP') or 99),
          'the viewer calls less often than the script answers (PROBE_GAP_SECONDS > PROBE_GAP: no call is dropped)')
    check(probe_gap_cpp is not None and max_probes is not None and probe_gap_cpp * max_probes >= start_answer,
          'the calls of a step (MAX_PROBES every PROBE_GAP_SECONDS) cover its wait')
    check(start_answer is not None and pair_wait >= start_answer + 20,
          'PAIR_WAIT outlasts the viewer\'s wait for the first answer (START_ANSWER_SECONDS) with room to spare')
    probe_fn = re.search(r'std::string probeText\(std::string_view objectKey\)\n    \{\n(.*?)\n    \}\n', proto_cpp, re.S)
    probe_body = probe_fn.group(1) if probe_fn else ''
    check("if (!isUuid(objectKey))\n        {\n            return std::string();\n        }" in probe_body
          and 'std::string text(PROBE_TEXT);' in probe_body and "text.push_back('|');" in probe_body
          and "(c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c" in probe_body and 'return text;' in probe_body
          and 'text != PROBE_TEXT + "|" + (string)llGetKey()' in lsl,
          'proto::probeText(key) is PROBE_TEXT, "|", the key in lower case (what llGetKey() writes), and empty for a key that is not a UUID: '
          'the script\'s text compare')
    send = cpp_function('void sendProbe(const LLUUID& bridge_object)')
    check('_PREHASH_ChatFromViewer' in send and 'CHAT_TYPE_WHISPER' in send and 'proto::PROBE_CHANNEL' in send
          and 'proto::probeText(bridge_object.asString())' in send and 'send_chat_from_viewer' not in send and 'LL_DEBUGS' in send
          and '++b.probeCount;' in send and 'b.nextProbeAt = now() + PROBE_GAP_SECONDS;' in send,
          'the call is a whisper of proto::probeText() on proto::PROBE_CHANNEL sent as ChatFromViewer, not through the RLV filter of '
          'send_chat_from_viewer, and the log has no text')
    due = cpp_function('void probeIfDue()')
    check('b.step == Step::None || b.step == Step::Detach || !b.url.empty() || b.probeCount >= MAX_PROBES ||' in due
          and 'now() < b.nextProbeAt || LLStartUp::getStartupState() < STATE_STARTED' in due
          and 'bridgeWornForCall()' in due and 'sendProbe(object->getID());' in due
          and 'recordedItem()' in cpp_function('LLViewerObject* bridgeWornForCall()')
          and 'getWornAttachment(item)' in cpp_function('LLViewerObject* bridgeWornForCall()'),
          'a call only while a step waits (not the take-off) and the URL is not held, at most MAX_PROBES, every '
          'PROBE_GAP_SECONDS, once logged in, while the bridge is worn (the recorded one before the steps found it)')
    check('        probeIfDue();\n        watchWorn();' in cpp_function('bool tick()'), 'the tick makes the calls')
    check('b.probeCount   = 0;' in cpp_function('void setStep(Step step, F64 wait_seconds)')
          and 'b.nextProbeAt  = 0.0;' in cpp_function('void setStep(Step step, F64 wait_seconds)'),
          'every step begins its calls again')
    check('b.probeCount   = 0;' in cpp_function('void leaveSteps()') and 'b.nextProbeAt  = 0.0;' in cpp_function('void leaveSteps()'),
          'leaving the steps ends the calls')
    find_item = cpp_function('void stepFindItem()')
    check('if (LLAppearanceMgr::instance().isLinkedInCOF(recorded))' in find_item
          and 'b.stepDeadline = now() + WEAR_SECONDS;' in find_item and 'if (!stepExpired())' in find_item
          and 'removeCOFItemLinks(recorded);' in find_item and 'b.goneOld.push_back({ recorded, now() + 2 * WEAR_SECONDS });' in find_item
          and re.search(r'if \(!b\.oldWaiting\)\n\s*\{\n\s*b\.oldWaiting\s*= true;\n\s*b\.stepDeadline = now\(\) \+ WEAR_SECONDS;\n\s*\}\n'
                        r'\s*if \(!stepExpired\(\)\)\n\s*\{\n\s*return;', find_item)
          and re.search(r'b\.oldWaiting\s*= false;', cpp_function('void setStep(Step step, F64 wait_seconds)'))
          and 0 <= find_item.find('removeCOFItemLinks(recorded);') < find_item.find('forgetItem();'),
          'a recorded old bridge whose wear is on the way is waited for like a wear (WEAR_SECONDS), then taken off; its COF link is '
          'removed and the item watched (goneOld) before it is forgotten (else the next login wears it again)')
    check(0 <= find_item.find('isOldBridgeName(it->getName())') < find_item.find('noteForeignBridges('),
          'the old version is looked at before the foreign bridges are noted (a wait does not log at every tick)')
    watch_gone = cpp_function('bool watchGoneOld()')
    check(re.search(r'removeCOFItemLinks\(it->id\);\n\s*LLVOAvatarSelf::detachAttachmentIntoInventory\(it->id\);\n'
                    r'\s*it = b\.goneOld\.erase\(it\);', watch_gone)
          and 'getWornAttachment(it->id)' in watch_gone and 'now() >= it->until' in watch_gone and 'bool gone_old = watchGoneOld();' in cpp_function('bool tick()')
          and 'gone_old;' in cpp_function('bool tick()'),
          'an old version\'s item that still comes after it was made anew is taken off, for 2 * WEAR_SECONDS (watched by the tick)')
    check('return (object && object->isHUDAttachment()) ? object : nullptr;' in cpp_function('LLViewerObject* bridgeWornForCall()'),
          'the call goes only to a bridge worn on a HUD point')
    check('repair(Repair::Rewear, "worn on the body");' in cpp_function('void stepWear()')
          and 'if (!object->isHUDAttachment())' in cpp_function('void stepWear()'),
          'a bridge worn on the body is put on the HUD again (a repair)')
    check('text != PROBE_TEXT + "|" + (string)llGetKey()' in lsl, 'the script takes a call only with its own key in it')
    check('if (found.notNull() && isOldBridgeName(it->getName()))' in cpp_function('void stepFindItem()')
          and 'takeOffOurselves(OwnDetach::Recreate);' in cpp_function('void stepFindItem()')
          and 'forgetItem();' in cpp_function('void stepFindItem()'),
          'a recorded bridge of an old version is not worn: made anew (taken off first when the login wore it)')
    check('return name == bridgeName() || isOldBridgeName(name);' in bridge_cpp
          and 'proto::isOldVersion(' in bridge_cpp and 'return version == "1.0" || version == "1.1";' in proto_cpp
          and 'constexpr char BRIDGE_VERSION[] = "1.2";' in proto_h,
          'the old versions 1.0 and 1.1 are tidied away with its object and script, and is not the current one')

    # ---- 2. lint
    try:
        tokens = tokenize(lsl)
    except SyntaxError as e:
        return fails + ['the script cannot be read: %s' % e]
    ops = [v for k, v in tokens if k == 'op']
    for a, b in ('{}', '()', '[]'):
        check(ops.count(a) == ops.count(b), 'balanced %s%s' % (a, b))
    try:
        globals_def, functions, events = Parser(lsl).script()
    except (SyntaxError, IndexError) as e:
        return fails + ['the script does not parse: %s' % e]
    names_declared = set()
    globals_ = {name for _, name, _ in globals_def}
    for name in globals_ | set(functions) | set(events):
        names_declared.add(name)
    for name in events:
        check(name in EVENTS, 'event %s is one of LSL\'s' % name)
        if name in EVENTS:
            check([p for p, _ in events[name][0]] == EVENTS[name], 'event %s has LSL\'s argument types' % name)

    def walk(node, out):
        if isinstance(node, tuple):
            out.append(node)
            for part in node[1:]:
                walk(part, out)
        elif isinstance(node, list):
            for part in node:
                walk(part, out)

    bodies = [(n, p, b) for n, (_, p, b) in functions.items()] + [(n, p, b) for n, (p, b) in events.items()]
    for owner, params, body in bodies:
        nodes = []
        walk(body, nodes)
        local_names = {name for ptype, name in params} | {n[2] for n in nodes if n[0] == 'local'}
        for name in local_names:
            check(name not in RESERVED, '%s: the name %s is a reserved word' % (owner, name))
            check(not name.startswith('ll'), '%s: the name %s looks like an LSL function' % (owner, name))
        for n in nodes:
            if n[0] == 'call':
                if n[1].startswith('ll'):
                    check(n[1] in LSL_FUNCTIONS, '%s: %s is not an LSL function' % (owner, n[1]))
                else:
                    check(n[1] in functions, '%s: function %s is defined' % (owner, n[1]))
            elif n[0] == 'name' or n[0] in ('pre', 'post'):
                name = n[1] if n[0] == 'name' else n[2]
                check(name in local_names or name in globals_ or name in CONSTANTS,
                      '%s: %s is declared (a parameter, a local, a global or an LSL constant)' % (owner, name))
            elif n[0] == 'assign':
                check(n[2] in local_names or n[2] in globals_, '%s: %s is declared before it is set' % (owner, n[2]))
    for vtype, name, _ in globals_def:
        check(name not in RESERVED and not name.startswith('ll'), 'global %s is not a reserved word' % name)
    for name in set(functions):
        check(name not in RESERVED and name not in EVENTS and not name.startswith('ll'), 'function %s has a free name' % name)

    # ---- 3. behaviour (the script's own text in the interpreter)
    try:
        behaviour(lsl, check)
    except Exception as e:   # a name the script lacks, a call the mock simulator does not know, ...
        fails.append('the script failed to run: %s: %s' % (type(e).__name__, e))
    return fails


# The lines of the sharing kinds (research/374, 374b), as handleSharingLine() reads (comments and spaces aside).
SHARING_LINE_BODY = r'''
            Bridge& b = bridge();
            if (!from_bridge)
            {
                // Always dropped, whoever it was and whatever the cause (the speaker's object not seen, or not
                // confirmed as the worn bridge for a moment): no script of the person's has a reason to say this form,
                // and these lines come 2 or more a second from every sharer near.
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of an object not confirmed as the worn bridge dropped" << LL_ENDL;
                return true;
            }
            if (b.cleanOf != chatter->getID())
            {
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: contents" << LL_ENDL;
                return true;
            }
            proto::ShareLine share;
            proto::PushLine  push;
            proto::PoseLine  reflect;
            LLUUID           peer;
            bool             ok = false;
            if (kind == proto::LineKind::Share)
            {
                ok = proto::parseShareLine(text, share);
                peer.set(share.peer);
            }
            else if (kind == proto::LineKind::Push)
            {
                ok = proto::parsePushLine(text, push);
                peer.set(push.peer);
            }
            else
            {
                ok = proto::parseReflectLine(text, reflect);
                peer.set(reflect.peer);
            }
            if (!ok)
            {
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: form" << LL_ENDL;
                return true;
            }
            if (peer == gAgentID)
            {
                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: self" << LL_ENDL;
                return true;
            }
            if (b.state == State::Off)
            {
                return true;
            }
            // Copies: a handler may set another or call back in.
            if (kind == proto::LineKind::Share)
            {
                const ShareHandler handler = b.shareHandler;
                if (handler)
                {
                    handler(peer, share.aud, share.seq, share.payload);
                }
            }
            else if (kind == proto::LineKind::Push)
            {
                const PushHandler handler = b.pushHandler;
                if (handler)
                {
                    handler(peer, push.seq, push.payload);
                }
            }
            else
            {
                const PoseHandler handler = b.reflectHandler;
                if (handler)
                {
                    handler(LLUUID(reflect.nonce), static_cast<PoseAction>(reflect.action), peer);
                }
            }
            return true;'''

URL_GRANT_MARGIN = 10.0  # seconds a simulator may take to grant a URL after the region change (a busy region)

# lslbridge.cpp's functions that move the user's items to the trash, that decide which worn object is the bridge, and
# that keep the take-off turns: (head, body, what). Each must read exactly so (comments and spaces aside).
PINNED = [
    ('F64 shareRateLeft(bool end)', r'''
        Bridge&           b      = bridge();
        const F64         t      = now();
        std::vector<F64>& window = end ? b.endLaunches : b.shareLaunches;
        const std::size_t most   = static_cast<std::size_t>(end ? MAX_ENDS_PER_WINDOW : MAX_SHARES_PER_WINDOW);
        window.erase(std::remove_if(window.begin(), window.end(), [t](F64 at) { return t - at >= SHARE_WINDOW_SECONDS || at > t; }),
                     window.end());
        if (window.size() < most)
        {
            return 0.0;
        }
        return window[window.size() - most] + SHARE_WINDOW_SECONDS - t;''',
     'the rate of the sharing is a sliding window of the launches: 4 shares and pushes, both lanes together, and 2 ends in their own window, in any second'),
    ('void laneAnswered()', r'''
        Bridge& b = bridge();
        if (b.laneFailSince > 0.0)
        {
            LL_INFOS("PlusLSLBridge") << "Sharing posts are answered again" << LL_ENDL;
            b.laneFailSince = 0.0;
        }''',
     'any answer of the bridge to a sharing post (ok, 429, another refusal) ends a run of failures'),
    ('void setKeyTime(SharingLane& lane, const std::string& key, F64 t)', r'''
        lane.lastPostAt.erase(std::remove_if(lane.lastPostAt.begin(), lane.lastPostAt.end(),
                                             [&key, t](const std::pair<std::string, F64>& e)
                                             { return e.first == key || t - e.second >= SHARE_SPACING_SECONDS; }),
                              lane.lastPostAt.end());
        lane.lastPostAt.emplace_back(key, t);''',
     'a key\'s time is kept for the spacing (replaced, and the ones past the spacing forgotten)'),
    ('F64 launchDelay(const SharingLane& lane, const std::string& key, bool end)', r'''
        return std::max(spacingLeft(lane, key), shareRateLeft(end));''',
     'a body waits for the spacing, and a share or push (not an end) also for the window'),
    ('void launchLane(PostKind kind, const std::string& key, const std::string& body, bool end)', r'''
        SharingLane& lane = laneOf(kind);
        const F64    t    = now();
        setKeyTime(lane, key, t);
        (end ? bridge().endLaunches : bridge().shareLaunches).push_back(t);
        lane.inFlight.push_back(key);
        PoseSent sent;
        sent.key = key;
        post(body, kind, sent);''',
     'a launch is noted for the key (the spacing) and, but for an end, in the window'),
    ('void urlLost()', r'''
        LL_INFOS("PlusLSLBridge") << "Bridge URL lost" << LL_ENDL;
        dropUrl();
        setStep(Step::AwaitAnswer, ANSWER_SECONDS);''',
     'a lost URL is dropped and waited for again'),
    ('void laneFailed()', r'''
        Bridge&   b = bridge();
        const F64 t = now();
        if (b.laneFailSince <= 0.0 || t - b.laneFailLast > LANE_FAIL_GAP_SECONDS)
        {
            b.laneFailSince = t;
            b.laneFailLast  = t;
            LL_INFOS("PlusLSLBridge") << "Sharing posts are failing (busy or timed out)" << LL_ENDL;
            return;
        }
        b.laneFailLast = t;
        if (t - b.laneFailSince >= LANE_FAIL_SECONDS)
        {
            LL_INFOS("PlusLSLBridge") << "Sharing posts failed for " << LANE_FAIL_SECONDS << " s" << LL_ENDL;
            urlLost();
        }''',
     'sharing posts failing: told when it starts, the URL lost when they all failed for LANE_FAIL_SECONDS'),
    ('F64 spacingLeft(const SharingLane& lane, const std::string& key)', r'''
        for (const auto& entry : lane.lastPostAt)
        {
            if (entry.first == key)
            {
                return entry.second + SHARE_SPACING_SECONDS - now();
            }
        }
        return 0.0;''',
     'the time left before a key\'s next sharing post is its last post\'s time plus the spacing'),
    ('void scheduleFlush(PostKind kind, const std::string& key, F64 delay)', r'''
        bool found = false;
        for (LaneWait& wait : laneOf(kind).waiting)
        {
            if (wait.key == key)
            {
                if (wait.scheduled)
                {
                    return;
                }
                wait.scheduled = true;
                found          = true;
            }
        }
        if (!found)
        {
            return;
        }
        const U32 epoch = bridge().epoch;
        doAfterInterval([kind, key, epoch]() { flushKey(kind, key, epoch); }, static_cast<F32>(std::max(delay, 0.0)));''',
     'what waits for a key is flushed once (a flag), after the delay, for the URL it was held for'),
    ('void flushKey(PostKind kind, const std::string& key, U32 epoch)', r'''
        Bridge& b = bridge();
        if (!LLApp::isRunning() || epoch != b.epoch)
        {
            return;
        }
        SharingLane& lane = laneOf(kind);
        const auto   wait = std::find_if(lane.waiting.begin(), lane.waiting.end(), [&key](const LaneWait& w) { return w.key == key; });
        if (wait == lane.waiting.end())
        {
            return;
        }
        wait->scheduled = false;
        if (std::find(lane.inFlight.begin(), lane.inFlight.end(), key) != lane.inFlight.end())
        {
            return;
        }
        if (lane.inFlight.size() >= MAX_LANE_POSTS)
        {
            scheduleFlush(kind, key, SHARE_SPACING_SECONDS); // 8 on their way: it goes when one has ended
            return;
        }
        const F64 left = launchDelay(lane, key, wait->end);
        if (left > 0.0)
        {
            scheduleFlush(kind, key, left);
            return;
        }
        const std::string next = wait->body;
        const bool        end  = wait->end;
        lane.waiting.erase(wait);
        if (laneReady())
        {
            launchLane(kind, key, next, end);
        }''',
     'a waiting body goes only for the URL held, when no post of its key is on the way and the spacing has passed'),
    ('bool laneReady()', r'''
        const Bridge& b = bridge();
        return b.state == State::Running && b.helloOk && !b.url.empty();''',
     'a sharing message goes only while Running, after the hello, with a URL'),
    ('bool queueLane(PostKind kind, const std::string& key, const std::string& body, bool end)', r'''
        if (body.empty() || !laneReady())
        {
            return false;
        }
        SharingLane& lane  = laneOf(kind);
        const bool   busy  = std::find(lane.inFlight.begin(), lane.inFlight.end(), key) != lane.inFlight.end();
        const F64    delay = launchDelay(lane, key, end);
        const bool   waits = busy || delay > 0.0;
        for (LaneWait& wait : lane.waiting)
        {
            if (wait.key == key)
            {
                if (wait.end && !end)
                {
                    return false;
                }
                wait.body = body;
                wait.end  = end;
                return true;
            }
        }
        if (waits)
        {
            if (lane.waiting.size() >= MAX_LANE_POSTS)
            {
                return false;
            }
            lane.waiting.push_back(LaneWait{key, body, end, false});
            if (!busy)
            {
                scheduleFlush(kind, key, delay);
            }
            return true;
        }
        if (lane.inFlight.size() >= MAX_LANE_POSTS)
        {
            return false;
        }
        launchLane(kind, key, body, end);
        return true;''',
     'a sharing body goes at once or waits in the single place of its key (the latest only, whatever the rate: a replacing body is never refused; an end is not replaced by a body that is not an end) while its key has a post on the way, its spacing has not passed or the window has no room; 8 on their way and 8 waiting at most'),
    ('void onLanePostDone(PostKind kind, const std::string& key, U32 epoch)', r'''
        Bridge& b = bridge();
        if (epoch != b.epoch)
        {
            return; // the lane was cleared with the URL it was for (dropUrl)
        }
        SharingLane& lane = laneOf(kind);
        const auto   in   = std::find(lane.inFlight.begin(), lane.inFlight.end(), key);
        if (in != lane.inFlight.end())
        {
            lane.inFlight.erase(in);
        }
        setKeyTime(lane, key, now());
        scheduleFlush(kind, key, spacingLeft(lane, key));''',
     'a sharing post that ended frees its key, notes the END as the key\'s time (the spacing counts from it) and schedules what waited for it, only for the URL still held'),
    ('bool sendShare(int aud, int seq, const std::string& payload)', r'''
        if (!laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Share, SHARE_KEY, proto::shareBody(bridge().token.asString(), aud, seq, payload), false);''',
     'a share is queued only when the lane is ready, under the key "share"'),
    ('bool sendShareEnd(int seq)', r'''
        if (!laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Share, SHARE_KEY, proto::shareEndBody(bridge().token.asString(), seq), true);''',
     'a share end is queued as an end, under the key "share"'),
    ('bool sendPush(const LLUUID& target, int seq, const std::string& payload)', r'''
        if (target.isNull() || target == gAgentID || !laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Push, target.asString(),
                         proto::pushBody(bridge().token.asString(), target.asString(), seq, payload), false);''',
     'a push is queued per target (never ourselves, never the null key)'),
    ('bool sendPushEnd(const LLUUID& target, int seq)', r'''
        if (target.isNull() || target == gAgentID || !laneReady())
        {
            return false;
        }
        return queueLane(PostKind::Push, target.asString(),
                         proto::pushEndBody(bridge().token.asString(), target.asString(), seq), true);''',
     'a push end is queued as an end per target'),
    ('bool isOurName(const std::string& name)', 'return name == bridgeName() || isOldBridgeName(name);',
     'only the exact names we give (and the old versions exact names) are ours (no prefix match)'),
    ('bool isOldBridgeName(const std::string& name)', r'''
        const std::string prefix(BRIDGE_NAME_PREFIX);
        return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
               proto::isOldVersion(std::string_view(name).substr(prefix.size()));''',
     'an old version name is the prefix and exactly an old version'),
    ('void removeOwnItem(const LLUUID& id)', r'''
        if (plus::invguard::blocksItemRemoval(id))
        {
            LL_INFOS("PlusLSLBridge") << "Bridge item kept: the inventory guards it" << LL_ENDL;
            return;
        }
        gInventory.removeItem(id);''',
     'an item goes to the trash (removeItem, not purged) only when no guard keeps it'),
    ('void forgetItem()', r'''
        Bridge& b = bridge();
        if (b.item.notNull() && gInventory.getItem(b.item) && !get_is_item_worn(b.item))
        {
            removeOwnItem(b.item);
        }
        if (b.item.notNull() && b.item == recordedItem())
        {
            recordItem(LLUUID::null);
        }
        b.item.setNull();''',
     'the bridge item made anew goes to the trash only when it is not worn, and its record goes'),
    ('void removeOtherBridgeItems()', r'''
        Bridge& b = bridge();
        if (b.folder.isNull() || !gInventory.isCategoryComplete(b.folder))
        {
            return;
        }
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(b.folder, cats, items);
        if (!items)
        {
            return;
        }
        uuid_vec_t gone;
        for (const LLPointer<LLViewerInventoryItem>& it : *items)
        {
            if (!it->getIsLinkType() && it->getType() == LLAssetType::AT_LSL_TEXT && it->getUUID() != b.item &&
                isOurName(it->getName()) && it->getCreatorUUID() == gAgentID)
            {
                gone.push_back(it->getUUID());
            }
        }
        for (const LLUUID& id : gone)
        {
            removeOwnItem(id);
        }
        if (!gone.empty())
        {
            LL_INFOS("PlusLSLBridge") << "Bridge folder: " << gone.size() << " old item(s) to the trash" << LL_ENDL;
        }''',
     'the folder is tidied only when loaded, and only of the script items under our exact name that we made: no '
     'object, no link, no script someone else made'),
    ('void noteForeignBridges(const LLInventoryModel::item_array_t& items, const LLUUID& ours)', r'''
        int foreign = 0;
        int worn    = 0;
        for (const LLPointer<LLViewerInventoryItem>& it : items)
        {
            if (it->getUUID() != ours && !it->getIsLinkType() && it->getType() == LLAssetType::AT_OBJECT &&
                isOurName(it->getName()))
            {
                ++foreign;
                worn += get_is_item_worn(it->getUUID()) ? 1 : 0;
            }
        }
        if (foreign > 0)
        {
            LL_WARNS("PlusLSLBridge") << "Bridge folder: " << foreign << " object(s) of the bridge's name that are not the "
                                      << "recorded bridge (" << worn << " worn): not used, left alone" << LL_ENDL;
        }''',
     'objects of the bridge\'s name that this viewer did not make are only told in the log (not worn, taken off or '
     'removed)'),
    ('LLUUID recordedItem()', r'''
        static LLCachedControl<std::string> recorded(gSavedPerAccountSettings, SETTING_ITEM, "");
        const std::string& text = recorded;
        LLUUID id;
        if (text.size() != static_cast<std::size_t>(UUID_STR_LENGTH - 1) || !LLUUID::validate(text) || !id.set(text, false))
        {
            return LLUUID::null;
        }
        return id;''',
     'the recorded bridge object is read from the account\'s settings and taken only when it is a whole UUID'),
    ('bool refuseLinkedParts(LLViewerObject* object)', r'''
        if (object->numChildren() <= 0)
        {
            return false;
        }
        LL_WARNS("PlusLSLBridge") << "Bridge object has " << object->numChildren() << " linked part(s): not used, made anew"
                                  << LL_ENDL;
        repair(Repair::Recreate, "linked parts");
        if (bridge().state == State::Failed && wornObject())
        {
            LLVOAvatarSelf::detachAttachmentIntoInventory(bridge().item);
        }
        return true;''',
     'a bridge with other prims linked to it is not used: made anew through a repair (so it ends), taken off when '
     'no repair is left'),
    ('void onPostResult(U32 epoch, PostKind kind, const PoseSent& sent, PostResult result)', r'''
        Bridge& b = bridge();
        const bool laneKind = (kind == PostKind::Share || kind == PostKind::Push);
        if (!laneKind && b.postsInFlight > 0)
        {
            --b.postsInFlight;
        }
        if (!LLApp::isRunning())
        {
            return;
        }
        if (epoch == b.epoch && b.state != State::Off) // else: for a URL dropped since
        {
            applyPostResult(kind, result);
        }
        if (laneKind)
        {
            onLanePostDone(kind, sent.key, epoch); // after applyPostResult: a URL it dropped cleared the lane
        }
        if (kind == PostKind::Pose || kind == PostKind::Reflect)
        {
            const bool ok = result == PostResult::Ok;
            const bool reflect = (kind == PostKind::Reflect);
            doOnIdleOneTime([sent, ok, reflect]()
            {
                // A copy: the handler may set another.
                const plus::lslbridge::SendResultHandler handler =
                    reflect ? bridge().reflectResultHandler : bridge().sendResultHandler;
                if (LLApp::isRunning() && handler)
                {
                    handler(sent.action, sent.nonce, sent.target, ok);
                }
            });
        }''',
     'every post\'s result: applied to the bridge only for the URL held; a pose message\'s HTTP result told once, as '
     'it was (also after the URL changed or Off), in the next idle, not while quitting'),
    ('bool watchLateWear()', r'''
        Bridge& b = bridge();
        if (b.wearAskedOf.isNull())
        {
            return false;
        }
        if (now() - b.wearAskedAt >= WEAR_SECONDS)
        {
            b.wearAskedOf.setNull();
            return false;
        }
        const bool stopped = b.state == State::Off || (b.state == State::Failed && b.retryAt <= 0.0);
        if (!stopped)
        {
            return false;
        }
        if (isAgentAvatarValid() && gAgentAvatarp->getWornAttachment(b.wearAskedOf))
        {
            LL_INFOS("PlusLSLBridge") << "Bridge worn after it stopped: taken off" << LL_ENDL;
            LLVOAvatarSelf::detachAttachmentIntoInventory(b.wearAskedOf);
            b.wearAskedOf.setNull();
            return false;
        }
        return true;''',
     'a late wear is watched WEAR_SECONDS (that first), only once stopped (Off, Failed with no new try), taken off '
     'when it comes, ownDetach left alone'),
    ('void dropUrl()', r'''
        Bridge& b = bridge();
        b.url.clear();
        b.token.setNull();
        b.helloSent         = false;
        b.helloOk           = false;
        b.transientFailures = 0;
        b.shareLane.clear(); // what was on the way or waiting was for the URL dropped
        b.pushLane.clear();
        b.shareLaunches.clear();
        b.endLaunches.clear();
        b.laneFailSince = 0.0;
        b.laneFailLast  = 0.0;
        ++b.epoch;''',
     'dropping the URL forgets it, its token and hello, clears the sharing lanes, and moves the epoch (late results are dropped)'),
    ('void leaveSteps()', r'''
        Bridge& b = bridge();
        dropUrl();
        cancelContents();
        b.step         = Step::None;
        b.stepDeadline = 0.0;
        b.probeCount   = 0;
        b.nextProbeAt  = 0.0;''',
     'every way out of Starting drops the URL and the contents asked for, and its calls'),
    ('void takeOffWhileOff()', r'''
        Bridge& b = bridge();
        if (wornObject())
        {
            b.ownDetach = OwnDetach::Off;
            LLVOAvatarSelf::detachAttachmentIntoInventory(b.item);
        }''',
     'a bridge worn while the setting is off is taken off'),
    ('PostResult classify(const LLSD& result)', r'''
        const LLSD               http   = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        if (status)
        {
            return (result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT].asString() == proto::REPLY_OK)
                       ? PostResult::Ok
                       : PostResult::Refused;
        }
        if (!status.isHttpStatus())
        {
            // curl's CURLE_PEER_FAILED_VERIFICATION (51 in older curl: the certificate does not name the host) and
            // CURLE_SSL_CACERT (60, its alias since curl 7.62: not trusted or not naming the host). Not transient: the
            // same at every try.
            if (status.getType() == LLCore::HttpStatus::EXT_CURL_EASY &&
                (status.getStatus() == CURL_PEER_NOT_VERIFIED_OLD || status.getStatus() == CURL_PEER_NOT_VERIFIED))
            {
                return PostResult::Certificate;
            }
            return PostResult::Transient; // timed out, the connection failed
        }
        const S32 code = status.getType();
        if (code >= 500)
        {
            return PostResult::Transient; // the simulator is busy
        }
        if (code == 429)
        {
            return PostResult::Rate; // the script's sharing rate (it says 429 for nothing else)
        }
        if (code == 403 || code == 404 || code == 410)
        {
            return PostResult::Gone;      // the URL is no more, or its script took another token (a new URL)
        }
        return PostResult::Refused;''',
     'a post is Ok only on "ok"; a certificate not verified (curl 51, 60) is Certificate; no answer or 5xx is '
     'Transient, 403/404/410 Gone, anything else Refused'),
    ('void onRefusedAnnouncement(proto::AnnouncementCheck check)', r'''
        Bridge& b = bridge();
        b.refusedInRow = (check == b.lastRefusal) ? b.refusedInRow + 1 : 1;
        b.lastRefusal  = check;
        if (b.refusedInRow >= MAX_REFUSED_ANNOUNCEMENTS && settingOn() && b.state != State::Off && b.state != State::Failed)
        {
            b.refusedInRow = 0;
            fail(Failure::Make, "announcement refused twice for the same reason");
        }''',
     'the same announcement refused MAX_REFUSED_ANNOUNCEMENTS times in a row is Failed (Make), not repaired'),
    ('void stop()', r'''
        Bridge& b = bridge();
        if (b.state == State::Off)
        {
            return;
        }
        if (wornObject())
        {
            b.ownDetach = OwnDetach::Off;
            LLVOAvatarSelf::detachAttachmentIntoInventory(b.item);
            if (b.wearAskedOf == b.item)
            {
                b.wearAskedOf.setNull();
            }
        }
        leaveSteps();
        b.attachConnection.disconnect();
        b.regionConnection.disconnect();
        b.notWornSince = 0.0;
        b.retryAt      = 0.0;
        b.waitingForPlace = false;
        setState(State::Off);
        if (b.wearAskedOf.notNull())
        {
            wake();
        }''',
     'the setting turned off takes the bridge off, leaves the steps, lets go of the signals, ends a wait for a place '
     'and watches a late wear'),
    ('void helloWhenClean()', r'''
        Bridge&         b      = bridge();
        LLViewerObject* object = wornObject();
        if (!object || b.url.empty() || b.helloSent || b.helloOk)
        {
            return;
        }
        if (refuseLinkedParts(object))
        {
            return;
        }
        if (object->getID() == b.cleanOf)
        {
            sendHello();
            return;
        }
        askContents(object);''',
     'one hello for a URL, to the worn bridge with no linked prim, only once its contents were seen clean'),
    ('bool takeRewear()', r'''
        Bridge&   b    = bridge();
        const F64 t    = now();
        F64*      slot = nullptr;
        for (F64& at : b.rewearAt)
        {
            if (at <= 0.0 || t - at >= REWEAR_WINDOW_SECONDS)
            {
                slot = &at;
                break;
            }
        }
        if (!slot)
        {
            return false;
        }
        *slot = t;
        return true;''',
     'a take-off is put on again at most MAX_REWEARS times in REWEAR_WINDOW_SECONDS'),
    ('bool isOurBridge(LLViewerObject* chatter)', r'''
        LLViewerObject* root = chatter->getRootEdit();
        if (!root || root != chatter || !root->isAttachment() || root->numChildren() > 0 || !isAgentAvatarValid())
        {
            return false;
        }
        const LLUUID worn_item = root->getAttachmentItemID();
        if (worn_item.isNull() || gAgentAvatarp->getWornAttachment(worn_item) != root)
        {
            return false;
        }
        Bridge& b = bridge();
        if (b.item.notNull())
        {
            return worn_item == b.item;
        }
        if (worn_item != recordedItem())
        {
            return false;
        }
        b.item = worn_item;
        return true;''',
     'a worn object is the bridge only as isOurBridge() reads (the root with no linked prim, worn, the bridge item; '
     'before it is known: the object recorded when this viewer made it, nothing found by its name)'),
]

PROBE_CHANNEL = 473520184      # proto::PROBE_CHANNEL (run_checks compares the script's own with proto.h)
PROBE_TEXT = 'charisBridgeProbe|1'
THIS = 'eeeeeeee-5555-4555-8555-eeeeeeeeeeee'   # the key of the worn bridge object (llGetKey)
PROBE = PROBE_TEXT + '|' + THIS                 # the call as the viewer says it (proto::probeText)
DIALOG_LABEL_MAX = 24                           # the longest label of an llDialog button
PAIR_WAIT_LSL = 60
ANNOUNCE_GAP_LSL = 15.0
OWNER = '1a2b3c4d-0000-4000-8000-00000000000a'
PEER = '5f2e8c41-9a7b-4d3c-8e16-2b4a9c7d1f03'
NONCE = '0b9a6d1e-4c2f-4e8a-9f31-7d5c2a1b6e40'
SENDER = 'aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa'    # the peer's attachment
URL = 'https://simhost-1.agni.secondlife.io:12043/cap/9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4'


def announcement(said):
    m = re.fullmatch(r'<charisBridge>([0-9.]+)\|([^|]*)\|([^|<]*)</charisBridge>', said)
    return m.groups() if m else None


def behaviour(lsl, check):
    def hear(s, w, speaker, text, channel=PROBE_CHANNEL):
        # What the simulator does: a listener hears a speaker its filter lets through (the script's own checks come after).
        for _, ch, filt in list(w.listens):
            if ch == channel and filt in (NULL_KEY, str(speaker)):
                s.fire('listen', channel, 'name', Key(speaker), text)
                return True
        return False

    def call(s, w):
        return hear(s, w, OWNER, PROBE)

    def worn(called=True):
        w = World(OWNER)
        w.objects[SENDER] = (PEER, 2)
        s = Script(lsl, w)
        s.fire('on_rez', 0)
        s.fire('attach', Key(OWNER))
        if called:
            call(s, w) # the viewer called before the URL came (nothing is said yet)
        return w, s

    def grant(w, s, url=URL):
        check(len(w.url_requests) > 0, 'a URL was asked for')
        if w.url_requests:
            s.fire('http_request', w.url_requests[-1], 'URL_REQUEST_GRANTED', url)

    def post(s, w, text):
        request = Key(str(uuid.uuid4()))
        s.fire('http_request', request, 'POST', '<llsd><string>%s</string></llsd>' % text)
        return w.responses.get(request, (None, None))[0]

    def token_of(w):
        found = [announcement(x) for x in w.said if announcement(x)]
        return found[-1][2] if found else None

    def hello(s, w):
        return post(s, w, 'hello|%s' % token_of(w))

    def pose_message(action='request', whom=OWNER, nonce=NONCE):
        return 'pose1|%s|%s|%s' % (nonce, action, whom)

    # -- worn: a URL, announced with a token; the timer for the next announcement
    w, s = worn()
    check(not w.temp_on_rez, 'a worn bridge is not made temporary')
    check(w.allow_drop == 0, 'nothing can be dropped into the worn bridge (llAllowInventoryDrop(FALSE))')
    grant(w, s)
    a = [announcement(x) for x in w.said]
    check(len(a) == 1 and a[0] and a[0][0] == '1.2' and a[0][1] == URL and key_is_true(a[0][2]), 'one announcement with the URL and a token')
    check(w.timer > 0, 'the next announcement is timed while no hello came')
    # -- no hello: announced ANNOUNCE_MAX times, then the bridge takes itself off
    s.fire('timer')
    s.fire('timer')
    check(len([x for x in w.said if announcement(x)]) == 3, 'announced 3 times in all while no hello came')
    check(not w.detached, 'not taken off before the third announcement went unanswered')
    s.fire('timer')
    check(w.detached, 'the bridge takes itself off when no viewer answered 3 announcements')

    # -- hello: connected, no more announcements; a pose1 for us is relayed only after it
    w, s = worn()
    grant(w, s)
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(not [x for x in w.said if x.startswith('<auroraPose>')], 'nothing relayed before the hello')
    check(post(s, w, 'pose|%s|request|%s|%s' % (token_of(w), NONCE, PEER)) == 403, 'a pose command before the hello: 403')
    check(hello(s, w) == 200, 'hello: 200')
    check(w.timer == 0, 'no announcement timed after the hello')
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(w.said[-1] == '<auroraPose>%s|request|%s</auroraPose>' % (NONCE, PEER), 'relayed with the owner the simulator knows')

    # -- a region crossed: a new URL and token, the hello asked again before anything is relayed (M2)
    old_token = token_of(w)
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    check(URL in w.released, 'the old URL is released')
    grant(w, s, URL.replace('simhost-1', 'simhost-2'))
    new_token = token_of(w)
    check(new_token and new_token != old_token, 'a new token with the new URL')
    check(w.timer > 0, 'the new URL is announced again until a hello comes')
    before = len(w.said)
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message('accept'))
    check(len(w.said) == before, 'nothing relayed between the new URL and its hello')
    check(post(s, w, 'hello|%s' % old_token) == 403, 'the old token is not taken any more')
    s.fire('timer')
    check(announcement(w.said[-1]) and announcement(w.said[-1])[2] == new_token, 'the new URL announced again after ANNOUNCE_GAP')
    check(hello(s, w) == 200 and w.timer == 0, 'hello for the new URL')
    for change in ('CHANGED_TELEPORT', 'CHANGED_REGION_START'):
        s.fire('changed', CONSTANTS[change])
        check(len(w.url_requests) and w.timer == 0, '%s: a new URL is asked for' % change)
        grant(w, s)
        check(w.timer > 0, '%s: announced until the hello' % change)
        hello(s, w)
    s.fire('on_rez', 0)
    check(post(s, w, 'pose|%s|request|%s|%s' % (token_of(w), NONCE, PEER)) == 403, 'on_rez: no hello for this rez yet')

    # -- a refused URL: told once without a secret, asked again after a while, then left
    w, s = worn()
    first = w.url_requests[-1]
    s.fire('http_request', first, 'URL_REQUEST_DENIED', '')
    check(w.said == ['<charisBridgeNoUrl>1.2</charisBridgeNoUrl>'], 'a refused URL is told once, with nothing secret')
    check(w.timer > 0, 'asked again after URL_RETRY_GAP')
    s.fire('timer')
    check(len(w.url_requests) == 2, 'the timer asks for a URL again')
    s.fire('http_request', w.url_requests[-1], 'URL_REQUEST_DENIED', '')
    check(len(w.said) == 1, 'a second refusal in a row is not told again')
    for _ in range(40):
        if w.timer > 0:
            s.fire('timer')
            s.fire('http_request', w.url_requests[-1], 'URL_REQUEST_DENIED', '')
    check(w.timer == 0 and len(w.url_requests) <= 31, 'at most URL_RETRY_MAX tries')

    # -- commands (viewer -> bridge)
    w, s = worn()
    grant(w, s)
    token = token_of(w)
    check(post(s, w, 'hello|00000000-0000-0000-0000-000000000000') == 403, 'hello with another token: 403')
    request = Key(str(uuid.uuid4()))
    s.fire('http_request', request, 'POST', 'hello|' + token)
    check(w.responses.get(request, (0,))[0] == 400, 'a body that is not an LLSD string: 400')
    check(post(s, w, 'hello|' + token.upper()) == 200, 'hello (the token in any case)')
    check(post(s, w, 'pose|%s|request|%s|%s' % (token, NONCE, PEER)) == 200, 'pose: 200')
    check(w.region_say == [(PEER, 'pose1|%s|request|%s' % (NONCE, PEER))], 'said to the target only, Aurora\'s format')
    for body, what in (('pose|%s|poke|%s|%s' % (token, NONCE, PEER), 'unknown action'),
                       ('pose|%s|request|%s|%s' % (token, NULL_KEY, PEER), 'null nonce'),
                       ('pose|%s|request|%s|%s' % (token, NONCE, OWNER), 'to ourselves'),
                       ('pose|%s|request|%s|%s|x' % (token, NONCE, PEER), 'six fields'),
                       ('pose|%s|request|%s' % (token, NONCE), 'four fields'),
                       ('nap|' + token, 'unknown command')):
        check(post(s, w, body) == 400, '%s: 400' % what)
    request = Key(str(uuid.uuid4()))
    s.fire('http_request', request, 'POST', '<llsd><string>hello|%s</string></llsd>' % token + ' ' * 1200)
    check(w.responses.get(request, (0,))[0] == 400, 'a body over BODY_MAX: 400')
    request = Key(str(uuid.uuid4()))
    s.fire('http_request', request, 'GET', '')
    check(w.responses.get(request, (0,))[0] == 405, 'GET: 405')
    check(len(w.region_say) == 1, 'nothing said for the refused ones')

    # -- pose1 heard (another avatar's attachment -> bridge -> viewer)
    w, s = worn()
    grant(w, s)
    hello(s, w)
    base = len(w.said)

    def relayed():
        return [x for x in w.said[base:] if x.startswith('<auroraPose>')]

    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(len(relayed()) == 1, 'a request relayed')
    w.time += 4
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(len(relayed()) == 1, 'the same asker\'s request within 5 s: dropped')
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message('accept'))
    check(len(relayed()) == 2, 'other actions are not limited')
    w.time += 1
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(len(relayed()) == 3, 'after 5 s: taken')
    w.time += 6
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message('cancel'))
    w.time += 2
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(len(relayed()) == 5, 'another action does not count toward the request gap (no short-circuit in LSL)')
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message('accept', OWNER.upper()))
    check(len(relayed()) == 6, 'the target in upper case is still us')
    count = len(relayed())
    for message, what in ((pose_message('accept', PEER), 'for someone else'),
                          (pose_message('poke'), 'unknown action'),
                          (pose_message('accept', OWNER, NULL_KEY), 'null nonce'),
                          ('pose2|%s|accept|%s' % (NONCE, OWNER), 'another tag'),
                          (pose_message('accept') + '|x', 'five fields'),
                          (pose_message('accept') + 'x' * 50, 'over 130 bytes')):
        s.fire('listen', -184735220, 'x', Key(SENDER), message)
        check(len(relayed()) == count, '%s: dropped' % what)
    rock = 'bbbbbbbb-2222-4222-8222-bbbbbbbbbbbb'
    w.objects[rock] = (PEER, 0)
    s.fire('listen', -184735220, 'x', Key(rock), pose_message('cancel'))
    check(len(relayed()) == count, 'from an object that is not worn: dropped')
    gone = 'cccccccc-3333-4333-8333-cccccccccccc'
    s.fire('listen', -184735220, 'x', Key(gone), pose_message('cancel'))
    check(len(relayed()) == count, 'from an object not in the region: dropped')
    ours = 'dddddddd-4444-4444-8444-dddddddddddd'
    w.objects[ours] = (OWNER, 2)
    s.fire('listen', -184735220, 'x', Key(ours), pose_message('cancel'))
    check(len(relayed()) == count, 'from our own attachment: dropped')
    w.point = 0
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message('cancel'))
    check(len(relayed()) == count, 'not worn: nothing relayed')

    # -- 64 askers kept, the first seen goes
    w, s = worn()
    grant(w, s)
    hello(s, w)
    base = len(w.said)
    for n in range(70):
        sender = '%08x-5555-4555-8555-555555555555' % (n + 1)
        w.objects[sender] = ('%08x-6666-4666-8666-666666666666' % (n + 1), 31)
        s.fire('listen', -184735220, 'x', Key(sender), pose_message())
    check(len(relayed()) == 70, 'every first request relayed')
    check(len(s.g['gAskers']) == 64 and len(s.g['gAskTimes']) == 64, 'at most 64 askers kept, in two lists of one length')
    check(str(s.g['gAskers'][0]).startswith('pose1|00000007'), 'the first seen dropped first')

    # -- sharing, reflect and push (research/374, 374b): what the viewer posts is said, what is heard is told
    SHARE_CH, REFLECT_CH, PUSH_CH = 473520191, 473520192, 473520193
    PAY = 'QUJDREVGR0g='                                   # base64 of "ABCDEFGH"
    LONGEST = 'A' * 900
    SENDER2 = 'aaaaaaaa-2222-4222-8222-aaaaaaaaaaaa'       # a second attachment of the same avatar
    PEER2 = '6a6a6a6a-1111-4111-8111-6a6a6a6a6a6a'
    SENDER3 = 'aaaaaaaa-3333-4333-8333-aaaaaaaaaaaa'       # another avatar's attachment

    def ready():
        w, s = worn()
        grant(w, s)
        hello(s, w)
        w.objects[SENDER2] = (PEER, 2)
        w.objects[SENDER3] = (PEER2, 2)
        return w, s

    def spoken(w):
        return list(w.spoken)

    # the channels are listened to (everyone is heard; the checks come after) and let go when taken off
    w, s = ready()
    heard_on = sorted(ch for _, ch, filt in w.listens if filt == NULL_KEY)
    check(SHARE_CH in heard_on and REFLECT_CH in heard_on and PUSH_CH in heard_on and -184735220 in heard_on,
          'the bridge listens on SHARE_CHANNEL, REFLECT_CHANNEL and PUSH_CHANNEL (no speaker filter), as on the pose channel')
    s.fire('attach', Key(''))
    check(not [1 for _, ch, _ in w.listens if ch in (SHARE_CH, REFLECT_CH, PUSH_CH)],
          'taken off: the share, reflect and push listeners are let go')

    # -- the viewer's share and shareend: shouted, only after the hello
    w, s = worn()
    grant(w, s)
    token = token_of(w)
    check(post(s, w, 'share|%s|1|5|%s' % (token, PAY)) == 403 and post(s, w, 'shareend|%s|5' % token) == 403
          and post(s, w, 'push|%s|%s|5|%s' % (token, PEER, PAY)) == 403 and post(s, w, 'pushend|%s|%s|5' % (token, PEER)) == 403
          and post(s, w, 'reflect|%s|request|%s|%s' % (token, NONCE, PEER)) == 403, 'share, push and reflect before the hello: 403')
    check(not spoken(w), 'nothing said before the hello')
    w, s = ready()
    token = token_of(w)
    check(post(s, w, 'share|%s|1|5|%s' % (token, PAY)) == 200, 'share: 200')
    check(spoken(w) == [('shout', SHARE_CH, '', 'share1|1|5|' + PAY)], 'a share is shouted on SHARE_CHANNEL in its own format')
    check(post(s, w, 'shareend|%s|9' % token) == 200 and spoken(w)[-1] == ('shout', SHARE_CH, '', 'share1|0|9|'),
          'a shareend is shouted as an empty payload for everyone')
    check(post(s, w, 'share|%s|0|999999999|%s' % (token.upper(), LONGEST)) == 200
          and spoken(w)[-1] == ('shout', SHARE_CH, '', 'share1|0|999999999|' + LONGEST)
          and len('<llsd><string>share|%s|0|999999999|%s</string></llsd>' % (token, LONGEST)) <= 1100,
          'the longest share (900 characters, 9 digits) goes through in one body')
    before = len(spoken(w))
    for body, what in (('share|%s|2|5|%s' % (token, PAY), 'aud 2'), ('share|%s||5|%s' % (token, PAY), 'no aud'),
                       ('share|%s|1|05|%s' % (token, PAY), 'seq with a leading zero'),
                       ('share|%s|1|-5|%s' % (token, PAY), 'a negative seq'), ('share|%s|1|+5|%s' % (token, PAY), 'seq with a sign'),
                       ('share|%s|1|1234567890|%s' % (token, PAY), 'a seq of 10 digits'),
                       ('share|%s|1|5x|%s' % (token, PAY), 'seq not a number'), ('share|%s|1||%s' % (token, PAY), 'no seq'),
                       ('share|%s|1|5|' % token, 'an empty payload (the end is shareend)'),
                       ('share|%s|1|5|QUJ' % token, 'a payload not a multiple of 4'),
                       ('share|%s|1|5|QU<D' % token, 'a payload with <'), ('share|%s|1|5|QU>D' % token, 'a payload with >'),
                       ('share|%s|1|5|QU D' % token, 'a payload with a space'),
                       ('share|%s|1|5|QU"D' % token, 'a payload with a quote'),
                       ('share|%s|1|5|%s' % (token, 'A' * 904), 'a payload over 900'),
                       ('share|%s|1|5' % token, 'four fields'), ('share|%s|1|5|%s|x' % (token, PAY), 'six fields'),
                       ('shareend|%s|x' % token, 'shareend with a bad seq'), ('shareend|%s' % token, 'shareend of two fields'),
                       ('shareend|%s|5|%s' % (token, PAY), 'shareend with a payload'),
                       ('share|%s|1|5|%s' % (NULL_KEY, PAY), 'another token')):
        status = post(s, w, body)
        check(status in (400, 403) and len(spoken(w)) == before, '%s: refused, nothing shouted' % what)

    # -- the viewer's push and pushend: said to the target only
    w, s = ready()
    token = token_of(w)
    check(post(s, w, 'push|%s|%s|3|%s' % (token, PEER, PAY)) == 200
          and spoken(w) == [('to', PUSH_CH, PEER, 'push1|3|' + PAY)], 'a push is said to the target on PUSH_CHANNEL only')
    check(post(s, w, 'pushend|%s|%s|4' % (token, PEER.upper())) == 200 and spoken(w)[-1] == ('to', PUSH_CH, PEER, 'push1|4|'),
          'a pushend is said as an empty payload')
    before = len(spoken(w))
    for body, what in (('push|%s|%s|3|%s' % (token, OWNER, PAY), 'a push to ourselves'),
                       ('push|%s|not-a-key|3|%s' % (token, PAY), 'a push to no key'),
                       ('push|%s|%s|3|%s' % (token, NULL_KEY, PAY), 'a push to the null key'),
                       ('push|%s|%s|3|' % (token, PEER), 'a push without a payload'),
                       ('push|%s|%s|3' % (token, PEER), 'a push of four fields'),
                       ('push|%s|%s|x|%s' % (token, PEER, PAY), 'a push with a bad seq'),
                       ('push|%s|%s|3|QU<D' % (token, PEER), 'a push payload with <'),
                       ('pushend|%s|%s|3|%s' % (token, PEER, PAY), 'a pushend of five fields'),
                       ('pushend|%s|%s' % (token, PEER), 'a pushend of three fields'),
                       ('pushend|%s|%s|3' % (token, OWNER), 'a pushend to ourselves')):
        check(post(s, w, body) == 400 and len(spoken(w)) == before, '%s: 400, nothing said' % what)

    # -- the viewer's reflect: said to the target on its own channel, the pose message as it was
    w, s = ready()
    token = token_of(w)
    check(post(s, w, 'reflect|%s|accept|%s|%s' % (token, NONCE, PEER)) == 200
          and spoken(w) == [('to', REFLECT_CH, PEER, 'reflect1|%s|accept|%s' % (NONCE, PEER))],
          'a reflect answer is said to the target on REFLECT_CHANNEL')
    check(post(s, w, 'pose|%s|accept|%s|%s' % (token, NONCE, PEER)) == 200
          and spoken(w)[-1] == ('to', -184735220, PEER, 'pose1|%s|accept|%s' % (NONCE, PEER)), 'a pose is still said on the pose channel')
    before = len(spoken(w))
    for body, what in (('reflect|%s|poke|%s|%s' % (token, NONCE, PEER), 'a reflect with an unknown action'),
                       ('reflect|%s|accept|%s|%s' % (token, NULL_KEY, PEER), 'a reflect with a null nonce'),
                       ('reflect|%s|accept|%s|%s' % (token, NONCE, OWNER), 'a reflect to ourselves'),
                       ('reflect|%s|accept|%s' % (token, NONCE), 'a reflect of four fields')):
        check(post(s, w, body) == 400 and len(spoken(w)) == before, '%s: 400' % what)

    # -- no more than SHOUT_MAX things said in a second
    w, s = ready()
    token = token_of(w)
    codes = [post(s, w, 'share|%s|0|%d|%s' % (token, n + 1, PAY)) for n in range(9)]
    check(codes == [200] * 6 + [429] * 3 and len(spoken(w)) == 6, 'at most 6 things said in a second (429 after, nothing said)')
    check(post(s, w, 'push|%s|%s|1|%s' % (token, PEER, PAY)) == 429, 'a push counts toward the same limit')
    w.clock += 1.0
    check(post(s, w, 'shareend|%s|9' % token) == 200 and len(spoken(w)) == 7, 'the next second says again')

    # -- a share heard: told to the owner's viewer, with the wearer the simulator knows
    w, s = ready()
    base = len(w.said)

    def told(tag):
        return [x for x in w.said[base:] if x.startswith('<' + tag + '>')]

    def hear_msg(channel, speaker, text):
        return hear(s, w, speaker, text, channel)

    hear_msg(SHARE_CH, SENDER, 'share1|1|5|' + PAY)
    check(told('charisShare') == ['<charisShare>1|%s|1|5|%s</charisShare>' % (PEER, PAY)],
          'a share is told with the peer the simulator gives (the wearer), aud, seq and payload')
    hear_msg(SHARE_CH, SENDER, 'share1|1|6|' + PAY)
    check(len(told('charisShare')) == 1, 'a second share of the same avatar within 0.3 s is dropped')
    hear_msg(SHARE_CH, SENDER2, 'share1|1|7|' + PAY)
    check(len(told('charisShare')) == 1, 'the gap is the avatar\'s, not the object\'s (a second attachment of the same avatar)')
    hear_msg(SHARE_CH, SENDER3, 'share1|0|1|' + PAY)
    check(len(told('charisShare')) == 2 and told('charisShare')[-1].startswith('<charisShare>1|%s|0|1|' % PEER2),
          'another avatar\'s share is taken at once')
    w.clock += 0.31
    hear_msg(SHARE_CH, SENDER, 'share1|0|8|')
    check(told('charisShare')[-1] == '<charisShare>1|%s|0|8|</charisShare>' % PEER, 'after the gap the same avatar is taken (an end: empty payload)')
    w.clock += 0.31
    count = len(told('charisShare'))
    for message, what in (('share1|2|5|' + PAY, 'aud 2'), ('share1|1|05|' + PAY, 'a seq with a leading zero'),
                          ('share1|1|-5|' + PAY, 'a negative seq'), ('share1|1|5|', 'an end for friends only'),
                          ('share1|1|5|QUJ', 'a payload not a multiple of 4'), ('share1|1|5|QU<D', 'a payload with <'),
                          ('share1|1|5|' + 'A' * 904, 'a payload over 900'), ('share1|1|5', 'two fields'),
                          ('share1|1|5|%s|x' % PAY, 'four fields'), ('share2|1|5|' + PAY, 'another version'),
                          (('<charisShare>1|%s|1|5|%s</charisShare>' % (OWNER, PAY)), 'a line written into the message')):
        w.clock += 0.31
        hear_msg(SHARE_CH, SENDER, message)
        check(len(told('charisShare')) == count, '%s: dropped' % what)
    # the peer is never read from the message
    w.clock += 0.31
    hear_msg(SHARE_CH, SENDER, 'share1|1|9|' + PAY)
    check(told('charisShare')[-1].split('|')[1] == PEER, 'the peer of a share is the wearer of the sending object')
    for obj, point, owner, what in (('bbbbbbbb-4444-4444-8444-bbbbbbbbbbbb', 0, PEER, 'an object on the ground'),
                                    ('dddddddd-5555-4555-8555-dddddddddddd', 2, OWNER, 'our own attachment')):
        w.objects[obj] = (owner, point)
        w.clock += 0.31
        count = len(told('charisShare'))
        hear_msg(SHARE_CH, obj, 'share1|1|5|' + PAY)
        check(len(told('charisShare')) == count, 'a share from %s: dropped' % what)
    w.clock += 0.31
    count = len(told('charisShare'))
    hear_msg(SHARE_CH, 'cccccccc-6666-4666-8666-cccccccccccc', 'share1|1|5|' + PAY)
    check(len(told('charisShare')) == count, 'a share from an object not in the region: dropped')
    w.point = 0
    w.clock += 0.31
    hear_msg(SHARE_CH, SENDER, 'share1|1|5|' + PAY)
    check(len(told('charisShare')) == count, 'not worn: nothing told')

    # a line of bars: dropped without making a list of it (the fields are cut out with llSubStringIndex)
    w, s = ready()
    base = len(w.said)
    hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
    for message, channel, what in (('share1|' + '|' * 953, SHARE_CH, 'a share of 953 bars'),
                                   ('push1|' + '|' * 953, PUSH_CH, 'a push of 953 bars'),
                                   ('share1|1|5|QUJD|QUJ', SHARE_CH, 'a share with a bar in its payload'),
                                   ('push1|5|QUJD|QUJ', PUSH_CH, 'a push with a bar in its payload'),
                                   ('share1|1||' + PAY, SHARE_CH, 'a share without a seq'),
                                   ('share1||5|' + PAY, SHARE_CH, 'a share without an aud'),
                                   ('push1||' + PAY, PUSH_CH, 'a push without a seq'),
                                   ('share1|1', SHARE_CH, 'a share with one field'), ('push1|', PUSH_CH, 'a push of nothing')):
        w.clock += 0.31
        hear_msg(channel, SENDER, message)
        check(not told('charisShare') and not told('charisPush'), '%s: dropped' % what)
    w.clock += 0.31
    hear_msg(SHARE_CH, SENDER, 'share1|1|5|' + PAY)
    check(len(told('charisShare')) == 1, 'a share that follows the refused ones is taken (nothing was kept of them)')

    # a share and a push have a gap each; a request and a reflect have a gap each
    w, s = ready()
    base = len(w.said)
    hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
    hear_msg(SHARE_CH, SENDER, 'share1|1|5|' + PAY)
    hear_msg(PUSH_CH, SENDER, 'push1|5|' + PAY)
    check(len(told('charisShare')) == 1 and len(told('charisPush')) == 1, 'a share and a push of one avatar do not use one gap')
    hear_msg(SHARE_CH, SENDER, pose_message())
    hear_msg(-184735220, SENDER, pose_message())
    hear_msg(REFLECT_CH, SENDER, 'reflect1|%s|request|%s' % (NONCE, OWNER))
    check(len(told('auroraPose')) == 1 and len(told('charisReflect')) == 1,
          'a request and a reflect request of one avatar do not use one gap')
    hear_msg(REFLECT_CH, SENDER, 'reflect1|%s|request|%s' % (NONCE, OWNER))
    check(len(told('charisReflect')) == 1, 'a second reflect request within 5 seconds is dropped')

    # -- a push heard
    w, s = ready()
    base = len(w.said)
    hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
    hear_msg(PUSH_CH, SENDER, 'push1|3|' + PAY)
    check(told('charisPush') == ['<charisPush>1|%s|3|%s</charisPush>' % (PEER, PAY)], 'a push is told with the wearer of the sending object')
    w.clock += 0.31
    hear_msg(PUSH_CH, SENDER, 'push1|4|')
    check(told('charisPush')[-1] == '<charisPush>1|%s|4|</charisPush>' % PEER, 'a push end is an empty payload')
    count = len(told('charisPush'))
    for message, what in (('push1|x|' + PAY, 'a bad seq'), ('push1|3|QUJ', 'a payload not a multiple of 4'),
                          ('push1|3|QU>D', 'a payload with >'), ('push1|3', 'one field'), ('push1|3|%s|x' % PAY, 'three fields'),
                          ('share1|3|' + PAY, 'a share message on the push channel'), ('push1|1|3|' + PAY, 'a share-shaped message')):
        w.clock += 0.31
        hear_msg(PUSH_CH, SENDER, message)
        check(len(told('charisPush')) == count, '%s: dropped' % what)
    w.clock += 0.31
    hear_msg(SHARE_CH, SENDER, 'push1|3|' + PAY)
    check(len(told('charisPush')) == count and not told('charisShare'), 'a push message on the share channel: dropped')

    # -- a reflect heard: the fields of a pose line
    w, s = ready()
    base = len(w.said)
    hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
    hear_msg(REFLECT_CH, SENDER, 'reflect1|%s|request|%s' % (NONCE, OWNER))
    check(told('charisReflect') == ['<charisReflect>%s|request|%s</charisReflect>' % (NONCE, PEER)],
          'a reflect request is told with the wearer as the peer')
    hear_msg(REFLECT_CH, SENDER, 'reflect1|%s|accept|%s' % (NONCE, OWNER))
    check(len(told('charisReflect')) == 2, 'an answer is not held by the request gap')
    count = len(told('charisReflect'))
    for message, what in (('reflect1|%s|accept|%s' % (NONCE, PEER), 'for someone else'),
                          ('reflect1|%s|poke|%s' % (NONCE, OWNER), 'an unknown action'),
                          ('reflect1|%s|accept|%s' % (NULL_KEY, OWNER), 'a null nonce'),
                          ('pose1|%s|accept|%s' % (NONCE, OWNER), 'a pose1 message on the reflect channel'),
                          ('reflect1|%s|accept|%s|x' % (NONCE, OWNER), 'five fields')):
        hear_msg(REFLECT_CH, SENDER, message)
        check(len(told('charisReflect')) == count, '%s: dropped' % what)
    hear_msg(-184735220, SENDER, 'reflect1|%s|accept|%s' % (NONCE, OWNER))
    check(len(told('charisReflect')) == count and not told('auroraPose'), 'a reflect message on the pose channel: dropped')

    # -- none of the three before the hello (a pose line is held back the same)
    w, s = worn()
    grant(w, s)
    base = len(w.said)
    hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
    hear_msg(SHARE_CH, SENDER, 'share1|1|5|' + PAY)
    hear_msg(PUSH_CH, SENDER, 'push1|5|' + PAY)
    hear_msg(REFLECT_CH, SENDER, 'reflect1|%s|request|%s' % (NONCE, OWNER))
    check(len(w.said) == base, 'nothing told before the hello')

    # -- at most GAP_MAX avatars kept; the gaps forgotten when worn again
    w, s = ready()
    base = len(w.said)
    hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
    for n in range(40):
        obj = '%08x-7777-4777-8777-777777777777' % (n + 1)
        w.objects[obj] = ('%08x-8888-4888-8888-888888888888' % (n + 1), 2)
        hear_msg(SHARE_CH, obj, 'share1|1|5|' + PAY)
    check(len(told('charisShare')) == 32 and len(s.g['gGapWho']) == 32 and len(s.g['gGapAt']) == 32,
          'at most 32 avatars kept for the share gap, in two lists of one length; the 33rd within the gap is not taken')
    w.clock += 0.31
    obj = '%08x-7777-4777-8777-777777777777' % 99
    w.objects[obj] = ('%08x-8888-4888-8888-888888888888' % 99, 2)
    hear_msg(SHARE_CH, obj, 'share1|1|5|' + PAY)
    check(len(told('charisShare')) == 33 and len(s.g['gGapWho']) == 32,
          'once the records are older than the gap, a new avatar takes the place of one of them')
    s.fire('attach', Key(OWNER))
    check(s.g['gGapWho'] == [] and s.g['gGapAt'] == [], 'the share gaps are forgotten when worn again')
    check(w.clock == 0.0, 'llGetTime() is started again when worn (a float kept small)')
    check(all(len([1 for _, ch, _ in w.listens if ch == c]) == 1 for c in (SHARE_CH, REFLECT_CH, PUSH_CH)),
          'worn again: one listener on each of the share, reflect and push channels (not added twice)')
    call(s, w)
    grant(w, s)
    hello(s, w)
    hear(s, w, SENDER, 'share1|1|5|' + PAY, SHARE_CH)
    check(len(s.g['gGapWho']) == 1, 'a share heard is kept for the gap')
    s.fire('attach', Key(''))
    check(s.g['gGapWho'] == [] and s.g['gGapAt'] == [], 'the share gaps are forgotten when taken off')

    # -- the table is full: the record that goes is the one older than the gap (not the first, not the last), the others stay
    for pos in (0, 15, 31):
        w, s = ready()
        base = len(w.said)
        hear_msg = lambda channel, speaker, text: hear(s, w, speaker, text, channel)
        objs = []
        for n in range(32):
            obj = '%08x-7777-4777-8777-777777777777' % (n + 200)
            w.objects[obj] = ('%08x-8888-4888-8888-888888888888' % (n + 200), 2)
            objs.append(obj)
        for obj in objs:                      # all at 0.0, in order
            hear_msg(SHARE_CH, obj, 'share1|1|5|' + PAY)
        w.clock += 0.31
        for n, obj in enumerate(objs):        # every one but pos again (older than the gap: taken, its record renewed)
            if n != pos:
                hear_msg(SHARE_CH, obj, 'share1|1|6|' + PAY)
        w.clock += 0.09                       # pos's record is 0.4 s old, the others' 0.09 s
        newcomer = '%08x-7777-4777-8777-777777777777' % 900
        w.objects[newcomer] = ('%08x-8888-4888-8888-888888888888' % 900, 2)
        before = len(told('charisShare'))
        hear_msg(SHARE_CH, newcomer, 'share1|1|7|' + PAY)
        check(len(told('charisShare')) == before + 1 and len(s.g['gGapWho']) == 32 and len(s.g['gGapAt']) == 32,
              'the table full: a new avatar takes the place of the one record older than the gap (at %d)' % pos)
        wearer_of = lambda n: '%08x-8888-4888-8888-888888888888' % (n + 200)
        stamps = {str(who): float(at) for who, at in zip(s.g['gGapWho'], s.g['gGapAt'])}
        check(len(stamps) == 32 and ('share1|' + wearer_of(pos)) not in stamps
              and ('share1|' + '%08x-8888-4888-8888-888888888888' % 900) in stamps
              and all(abs(stamps['share1|' + wearer_of(n)] - 0.31) < 1e-6 for n in range(32) if n != pos)
              and abs(stamps['share1|' + '%08x-8888-4888-8888-888888888888' % 900] - w.clock) < 1e-6,
              'the table full: only the old record went (pos %d), the others keep their own times, the lists stay in step' % pos)
        before = len(told('charisShare'))
        for n, obj in enumerate(objs):
            if n != pos:
                hear_msg(SHARE_CH, obj, 'share1|1|8|' + PAY)
        check(len(told('charisShare')) == before, 'the records within the gap stay: every other avatar\'s next share within it is dropped (pos %d)' % pos)

    # -- another script in the bridge is stopped as soon as it starts (it could say a URL of its own)
    w = World(OWNER)
    w.scripts.append('Other')
    s = Script(lsl, w)
    s.fire('attach', Key(OWNER))
    check('Other' in w.stopped, 'another script is stopped when the bridge starts, not only when the contents change')

    # -- worn again without a detach in between (a login): the askers are forgotten
    w, s = worn()
    grant(w, s)
    hello(s, w)
    base = len(w.said)
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    w.time += 1
    s.fire('attach', Key(OWNER))
    call(s, w)
    grant(w, s)
    hello(s, w)
    s.fire('listen', -184735220, 'x', Key(SENDER), pose_message())
    check(len(relayed()) == 2, 'wearing it again forgets the askers (a request within 5 s of the last one is taken)')

    # -- a grant for an older URL request, a grant while not worn: released, not announced; taken off: released
    w, s = worn()
    first = w.url_requests[-1]
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    s.fire('http_request', first, 'URL_REQUEST_GRANTED', URL)
    check(URL in w.released and not [x for x in w.said if announcement(x)], 'a grant for an older request is released')
    grant(w, s, URL.replace('simhost-1', 'simhost-3'))
    check(len([x for x in w.said if announcement(x)]) == 1, 'the grant for the last request is announced')
    w, s = worn()
    w.point = 0
    s.fire('http_request', w.url_requests[-1], 'URL_REQUEST_GRANTED', URL)
    check(URL in w.released and not w.said, 'a grant that comes while not worn is released')
    w, s = worn()
    grant(w, s)
    w.point = 0
    s.fire('attach', Key(NULL_KEY))
    check(URL in w.released, 'taken off: its URL is released')

    # -- the hello that comes after it asked to take itself off, before that was granted: it stays
    w, s = worn()
    w.hold_perms = True
    grant(w, s)
    for _ in range(3):
        s.fire('timer')
    check(w.perms and w.held, 'asked to take itself off after 3 announcements')
    check(hello(s, w) == 200, 'a late hello is still taken')
    for ev, args in w.held:
        s.fire(ev, *args)
    check(not w.detached, 'a hello before the permission came keeps the bridge on')

    # -- not worn: the timer and a region change do nothing (no announcement, no URL asked for, no take-off)
    w, s = worn()
    grant(w, s)
    w.point = 0
    said, asked, perms = len(w.said), len(w.url_requests), len(w.perms)
    s.fire('timer')
    check(len(w.said) == said and len(w.url_requests) == asked and len(w.perms) == perms, 'the timer does nothing while not worn')
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    check(len(w.url_requests) == asked, 'a region change while not worn asks for no URL')
    w, s = worn(called=False)
    grant(w, s)
    w.point = 0
    w.time += 100
    perms = len(w.perms)
    s.fire('timer')
    check(len(w.perms) == perms and not w.detached, 'the timer does nothing while not worn, even past the call deadline')

    # -- the bridge speaks only to a viewer that called (research/366): another viewer would show its lines in the chat
    w, s = worn(called=False)
    check((PROBE_CHANNEL, str(OWNER)) in [(l[1], l[2]) for l in w.listens],
          'it listens on the call channel, to the owner avatar only')
    check(not [l for l in w.listens if l[1] == PROBE_CHANNEL and l[2] != str(OWNER)], 'no listener on the call channel without the owner filter')
    grant(w, s)
    check(w.said == [], 'worn and given a URL, it says nothing before a call (not the URL, not the token)')
    check(w.timer > 0 and w.timer <= PAIR_WAIT_LSL, 'a deadline is timed for the call')
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    grant(w, s, URL.replace('simhost-1', 'simhost-2'))
    check(w.said == [], 'a region change before any call: still nothing said')
    check(len(PROBE) > DIALOG_LABEL_MAX, 'a call is longer than a dialog button label (24 bytes): the owner cannot be made to press one')
    check(not hear(s, w, PEER, PROBE), 'another avatar\'s call is not heard (the listener filters by speaker)')
    check(not hear(s, w, SENDER, PROBE), 'another object\'s call is not heard')
    for who in (PEER, SENDER, OWNER.replace('1', '2')):
        s.fire('listen', PROBE_CHANNEL, 'x', Key(who), PROBE)  # even if the filter let it through
    check(w.said == [] and s.g['gPaired'] == 0, 'a call of anyone but the owner avatar does nothing (second check in the script)')
    # What the owner's avatar says when they press the button of someone's dialog (the label, at most 24 bytes: the call
    # without the key is 19), or what they type: the key of this bridge is what a call must hold.
    for text in (PROBE_TEXT, 'charisBridgeProbe|2|' + THIS, PROBE + ' ', PROBE_TEXT + '|' + NULL_KEY, PROBE.upper(),
                 PROBE_TEXT + '|' + SENDER, PROBE_TEXT + '|' + OWNER, 'charisBridgeProbe', '', 'hello', THIS):
        hear(s, w, OWNER, text)
    check(len(PROBE_TEXT) <= DIALOG_LABEL_MAX, 'the bare call text fits a dialog button (so the key must be checked)')
    check(w.said == [] and s.g['gPaired'] == 0, 'a call of another text, or without this object\'s key, does nothing')
    check(call(s, w), 'the owner\'s call is heard')
    a = [announcement(x) for x in w.said]
    check(len(a) == 1 and a[0] and a[0][0] == '1.2' and a[0][1] == URL.replace('simhost-1', 'simhost-2') and key_is_true(a[0][2]),
          'the call is answered at once with the URL held')
    check(w.timer == ANNOUNCE_GAP_LSL, 'then announced again every ANNOUNCE_GAP until the hello')
    call(s, w)
    check(len(w.said) == 1, 'a second call within PROBE_GAP is not answered')
    for _ in range(50):
        call(s, w)
    check(len(w.said) == 1, 'a flood of calls in one second: one answer')
    w.time += 2
    call(s, w)
    check(len(w.said) == 2, 'a call after PROBE_GAP is answered again (the viewer lost the line)')
    check(hello(s, w) == 200 and w.timer == 0, 'the hello after a call')
    w.time += 500
    s.fire('timer')
    check(not w.detached, 'a bridge that was called and answered stays on')

    # -- the deadline is from the wear: a region change does not extend it; a call while not worn does nothing
    w, s = worn(called=False)
    grant(w, s)
    w.time += 40
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    grant(w, s, URL.replace('simhost-1', 'simhost-2'))
    w.time += 20
    s.fire('timer')
    check(w.detached and w.said == [], 'no call: taken off PAIR_WAIT after the wear, not after the last region change')
    w, s = worn(called=False)
    w.point = 0
    call(s, w)
    check(s.g['gPaired'] == 0 and w.said == [], 'a call while not worn does nothing')

    # -- worn on the body, the bridge's key is on every list (llGetAttachedList) and a link in the chat can make the owner say
    # any text: a call is taken on a HUD point only (31 to 38)
    for point in (1, 2, 30, 31, 32, 35, 38, 39):
        w, s = worn(called=False)
        w.point = point
        grant(w, s)
        call(s, w)
        hud = 31 <= point <= 38
        check((s.g['gPaired'] == 1) == hud and (len(w.said) == 1) == hud,
              'attach point %d: a call is %s' % (point, 'taken (a HUD point)' if hud else 'not taken (the body: its key is public)'))

    # -- called on the HUD, then moved by hand to the body while it runs: the bridge says nothing more (not a new URL's
    # announcement, not the no-URL line, not an answer to a call); the viewer's wait for it ends Silent and repairs it
    w, s = worn()
    grant(w, s)
    hello(s, w)
    said = len(w.said)
    w.point = 2
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    grant(w, s, URL.replace('simhost-1', 'simhost-4'))
    call(s, w)
    check(len(w.said) == said, 'moved to the body while running: a new URL is not announced')
    s.fire('changed', CONSTANTS['CHANGED_REGION'])
    s.fire('http_request', w.url_requests[-1], 'URL_REQUEST_DENIED', '')
    check(len(w.said) == said, 'moved to the body while running: the no-URL line is not said')
    for _ in range(4):
        w.time += 20
        s.fire('timer')
    check(len(w.said) == said, 'on the body no timer repeat says anything either')

    # -- the announcements an answer to a call does not count
    w, s = worn(called=False)
    grant(w, s)
    call(s, w)
    for _ in range(3):
        w.time += 2
        call(s, w)
    for _ in range(3):
        s.fire('timer')
    check(not w.detached and len([x for x in w.said if announcement(x)]) == 7,
          'answers to calls are not counted: the timer still announces 3 times before the bridge goes')
    s.fire('timer')
    check(w.detached, 'then the bridge takes itself off')

    # -- a call before the URL came: told when it comes; a refused URL: told only after a call
    w, s = worn(called=False)
    call(s, w)
    check(w.said == [], 'a call before the URL: nothing yet')
    grant(w, s)
    check(len([x for x in w.said if announcement(x)]) == 1, 'the URL that comes after a call is announced')
    w, s = worn(called=False)
    s.fire('http_request', w.url_requests[-1], 'URL_REQUEST_DENIED', '')
    check(w.said == [], 'a refused URL is not told before a call (the line would show in another viewer)')
    call(s, w)
    check(w.said == ['<charisBridgeNoUrl>1.2</charisBridgeNoUrl>'], 'a call after a refused URL is answered with the no-URL line')

    # -- no call at all (another viewer): nothing said, taken off after PAIR_WAIT
    w, s = worn(called=False)
    grant(w, s)
    w.time += PAIR_WAIT_LSL // 2
    s.fire('timer')
    check(not w.detached and w.timer > 0, 'before PAIR_WAIT the bridge waits')
    w.time += PAIR_WAIT_LSL // 2
    s.fire('timer')
    check(w.detached and w.said == [], 'no call within PAIR_WAIT: taken off, having said nothing')
    w, s = worn(called=False)
    w.hold_perms = True
    w.time += PAIR_WAIT_LSL
    s.fire('timer')
    check(w.perms and w.held, 'no call: asked to take itself off')
    call(s, w)
    for ev, args in w.held:
        s.fire(ev, *args)
    check(w.detached, 'a call after the deadline does not keep a bridge that asked to go (nothing was connected)')

    # -- worn again, rezzed, taken off: the call is forgotten
    for how in ('attach', 'rez', 'off'):
        w, s = worn()
        grant(w, s)
        check(s.g['gPaired'] == 1, 'called')
        if how == 'attach':
            s.fire('attach', Key(OWNER))
        elif how == 'rez':
            s.fire('on_rez', 0)
        else:
            w.point = 0
            s.fire('attach', Key(NULL_KEY))
            check(not [l for l in w.listens if l[1] == PROBE_CHANNEL], 'taken off: the call listener is removed')
        check(s.g['gPaired'] == 0, '%s: the call is forgotten' % how)
    w, s = worn()
    s.fire('attach', Key(OWNER))
    n = len(w.said)
    grant(w, s)
    check(len(w.said) == n, 'worn again: nothing is said before the viewer calls again')
    check(len([l for l in w.listens if l[1] == PROBE_CHANNEL]) == 1, 'worn again: one call listener, not two')

    # -- bodies at the edges
    w, s = worn()
    grant(w, s)
    token = token_of(w)
    check(post(s, w, 'hello|%s|x' % token) == 400, 'a hello with three fields: 400')
    check(post(s, w, '') == 400, 'an empty string: 400')
    w2 = World(OWNER)
    w2.point = 0
    s2 = Script(lsl, w2)
    s2.fire('state_entry')
    check(post(s2, w2, 'hello|') == 403, 'before any URL (no token yet) an empty token is not taken')

    # -- other scripts stopped, owner change resets, taken off: stopped
    w, s = worn()
    w.scripts.append('Other')
    s.fire('changed', CONSTANTS['CHANGED_INVENTORY'])
    check(w.stopped == ['Other'], 'another script in the bridge is stopped')
    grant(w, s)
    hello(s, w)
    s.fire('changed', CONSTANTS['CHANGED_OWNER'])
    check(s.g['gConnected'] == 0 and len(w.url_requests) >= 2, 'a new owner: the script starts again')
    w.point = 0
    s.fire('attach', Key(NULL_KEY))
    check(s.g['gConnected'] == 0 and w.timer == 0, 'taken off: stopped')
    w2 = World(OWNER)
    w2.point = 0
    s2 = Script(lsl, w2)
    s2.fire('on_rez', 0)
    check(w2.temp_on_rez, 'put on the ground: made temporary')


MUTANTS = [
    ('askForUrl keeps the old hello (M2)', '    releaseUrl();\n    gConnected = FALSE;\n', '    releaseUrl();\n'),
    ('on_rez keeps the old hello', '        gConnected = FALSE; // a new rez', '        // a new rez'),
    ('a typo in an LSL function', 'llOwnerSay(open + id', 'llOwnerSays(open + id'),
    ('a local used before any declaration', 'string kind = llList2String(parts, 1);', 'string kinds = llList2String(parts, 1);'),
    ('a reserved word as a name', 'integer count = llGetListLength(words);', 'integer state = llGetListLength(words);'),
    ('an event with a wrong argument', 'listen(integer channel, string name, key id, string message)', 'listen(integer channel, key name, key id, string message)'),
    ('no taking itself off', '        llRequestPermissions(gOwner, PERMISSION_ATTACH);\n', '\n'),
    ('announcements too far apart for the viewer', 'float   ANNOUNCE_GAP  = 15.0;', 'float   ANNOUNCE_GAP  = 25.0;'),
    ('the request gap counted for every action (&& without short-circuit)',
     '    if (kind == "request")\n    {\n        if (!requestTaken(head + (string)asker))\n        {\n            return;\n        }\n    }\n',
     '    if (kind == "request" && !requestTaken(head + (string)asker))\n    {\n        return;\n    }\n'),
    ('relayed from things put on the ground', '    if (llList2Integer(point, 0) < 1)\n', '    if (llList2Integer(point, 0) < 0)\n'),
    ('relayed before the hello', '    if (!gConnected || !llGetAttached())\n    {\n        return;\n    }\n    integer skip = llStringLength(head);\n    if (llStringLength(text) > POSE_MAX',
     '    if (!llGetAttached())\n    {\n        return;\n    }\n    integer skip = llStringLength(head);\n    if (llStringLength(text) > POSE_MAX'),
    ('the token not checked', 'if (!isKey((string)gToken) || llToLower(llList2String(words, 1)) != (string)gToken)',
     'if (!isKey((string)gToken))'),
    ('no-URL told at every refusal', '                if (gUrlTries == 0)\n', '                if (TRUE)\n'),
    ('other scripts not stopped when it starts', '    stopOtherScripts(); // one put in', '    // one put in'),
    ('the askers kept when it is worn again', '    gAskers = [];\n    gAskTimes = [];\n    gGapWho = [];\n    gGapAt = [];\n    gShoutCount = 0;\n    llResetTime();',
     '    gGapWho = [];\n    gGapAt = [];\n    gShoutCount = 0;\n    llResetTime();'),
    # -- the review's fixes (research/374, pose-share-bridge review 1)
    ('a gap record deleted from one list only', '            gGapAt = llDeleteSubList(gGapAt, found, found);', '            gGapAt = llDeleteSubList(gGapAt, 0, 0);'),
    ('a gap record newer than the smaller age evicted', '                if (stamp - llList2Float(gGapAt, old) >= SHARE_GAP)', '                if (stamp - llList2Float(gGapAt, old) >= 0.05)'),
    ('a gap record evicted without its age (the first)', '                    found = old;', '                    found = 0;'),
    ('the first gap record never looked at', '            while (old > 0 && found == -1)', '            while (old > 1 && found == -1)'),
    ('a heard share split into a list again', '    string rest = llDeleteSubString(text, 0, skip - 1);\n',
     '    string rest = llDeleteSubString(text, 0, skip - 1);\n    list junk = llParseStringKeepNulls(rest, ["|"], []);\n'),
    ('a bar in a heard payload taken', '    if (llSubStringIndex(payload, "|") != -1)\n    {\n        return; // one field too many\n    }\n', ''),
    ('a gap record within the gap evicted', '                if (stamp - llList2Float(gGapAt, old) >= SHARE_GAP)', '                if (TRUE)'),
    ('a new avatar taken when no gap record is old', '            if (found == -1)\n            {\n                return FALSE;\n            }\n', ''),
    ('llGetTime() not restarted at a wear', '    llResetTime(); // llGetTime() is a float', '    // llGetTime() is a float'),
    # -- sharing, reflect and push (research/374, 374b)
    ('a share relayed before the hello', '    if (!gConnected || !llGetAttached())\n    {\n        return;\n    }\n    string head = "push1|";',
     '    if (!llGetAttached())\n    {\n        return;\n    }\n    string head = "push1|";'),
    ('a share said before the hello', '    if (!gConnected)\n    {\n        reply(request, 403, "hello");\n        return;\n    }\n    // share|<token>',
     '    // share|<token>'),
    ('the peer of a share the sending object, not its wearer', '    key sender = wearerOf(thing);', '    key sender = thing;'),
    ('no gap between one avatar\'s shares', '    if (!gapTaken(head + (string)sender))', '    if (FALSE)'),
    ('one gap for shares and pushes', '    if (!gapTaken(head + (string)sender))', '    if (!gapTaken((string)sender))'),
    ('the gap kept per object, not per avatar', '    if (!gapTaken(head + (string)sender))', '    if (!gapTaken(head + (string)thing))'),
    ('a gap of 0.3 s that is not one', 'float   SHARE_GAP     = 0.3;', 'float   SHARE_GAP     = 0.0;'),
    ('the gap list not bounded', '    if (llGetListLength(gGapWho) >= GAP_MAX)', '    if (FALSE)'),
    ('the gap not forgotten at a take-off', '    gGapWho = [];\n    gGapAt = [];\n    llSetTimerEvent(0.0);', '    llSetTimerEvent(0.0);'),
    ('an end for friends only taken', '!payloadOk(payload, aud == "0")', '!payloadOk(payload, TRUE)'),
    ('no limit on what is said', '    if (!saidAllowed())', '    if (FALSE)'),
    ('a limit too high', 'integer SHOUT_MAX     = 6;', 'integer SHOUT_MAX     = 600;'),
    ('the limit not restarted by the second', '    if (stamp - gShoutWindow >= 1.0 || stamp < gShoutWindow)', '    if (FALSE)'),
    ('a share shouted on the call channel', 'llShout(SHARE_CHANNEL, "share1|"', 'llShout(PROBE_CHANNEL, "share1|"'),
    ('a push shouted to everyone', 'llRegionSayTo((key)whom, PUSH_CHANNEL, "push1|"', 'llShout(PUSH_CHANNEL, "push1|"'),
    ('a push to ourselves or no key taken', '    if (toOne && (!isKey(whom) || whom == (string)gOwner))', '    if (FALSE)'),
    ('a reflect said on the pose channel', '        channel = REFLECT_CHANNEL;', '        channel = POSE_CHANNEL;'),
    ('one request gap for pose and reflect', '        if (!requestTaken(head + (string)asker))', '        if (!requestTaken((string)asker))'),
    ('a negative sequence number', '    if (llGetSubString(text, 0, 0) == "-")', '    if (FALSE)'),
    ('a sequence number of any digits', '    if (size < 1 || size > SEQ_DIGITS_MAX)', '    if (size < 1)'),
    ('a sequence number that is not a number', '    return ((string)((integer)text) == text);', '    return TRUE;'),
    ('a payload of any characters', 'list    BAD_CHARS     = ["<", ">", "\\n", " ", "\\""];', 'list    BAD_CHARS     = [];'),
    ('a payload with a space allowed', 'list    BAD_CHARS     = ["<", ">", "\\n", " ", "\\""];', 'list    BAD_CHARS     = ["<", ">", "\\n", "\\""];'),
    ('a payload not a multiple of 4', ' || size % 4 != 0)', ')'),
    ('a payload of any length', '    if (size > PAYLOAD_MAX || size % 4 != 0)', '    if (size % 4 != 0)'),
    ('another SHARE_CHANNEL', 'integer SHARE_CHANNEL   = 473520191;', 'integer SHARE_CHANNEL   = 473520184;'),
    ('a body limit over the viewer\'s', 'integer BODY_MAX      = 1100; ', 'integer BODY_MAX      = 4000; '),
    ('the share listener not let go when taken off', '    llListenRemove(gShareListen);\n    gShareListen = 0;\n', ''),
    ('the push listener not let go when taken off', '    llListenRemove(gPushListen);\n    gPushListen = 0;\n', ''),
    ('the reflect listener not let go when taken off', '    llListenRemove(gReflectListen);\n    gReflectListen = 0;\n', ''),
    ('the share listener added again at every wear', '    llListenRemove(gShareListen);\n    gShareListen = llListen(', '    gShareListen = llListen('),
    ('no share listener', '    gShareListen = llListen(SHARE_CHANNEL, "", NULL_KEY, "");\n', ''),
    ('no push listener', '    gPushListen = llListen(PUSH_CHANNEL, "", NULL_KEY, "");\n', ''),
    ('a share told as a push', 'llOwnerSay("<charisShare>1|"', 'llOwnerSay("<charisPush>1|"'),
    ('a share heard on the push channel', '            relayPayload(id, message, FALSE);', '            relayPayload(id, message, TRUE);'),
    ('a pose message taken on the reflect channel', '            relayRequest(id, message, "reflect1|", "<charisReflect>", "</charisReflect>");',
     '            relayRequest(id, message, "pose1|", "<charisReflect>", "</charisReflect>");'),
    ('a grant for an older URL request taken', 'if (request != gUrlRequest || !llGetAttached())', 'if (!llGetAttached())'),
    ('a grant taken while not worn', 'if (request != gUrlRequest || !llGetAttached())', 'if (request != gUrlRequest)'),
    ('the URL kept when taken off', '    llSetTimerEvent(0.0);\n    releaseUrl();\n}', '    llSetTimerEvent(0.0);\n}'),
    ('taken off although a hello came before the permission', 'if ((granted & PERMISSION_ATTACH) && !gConnected)',
     'if (granted & PERMISSION_ATTACH)'),
    ('a hello of any number of fields', 'if (verb == "hello" && count == 2)', 'if (verb == "hello")'),
    ('an empty string taken', 'upto <= from + 8', 'upto < from + 8'),
    ('the token compared before any URL', 'if (!isKey((string)gToken) ||', 'if (gToken == NULL_KEY ||'),
    ('the call listener without the owner filter', 'gProbeListen = llListen(PROBE_CHANNEL, "", gOwner, "");',
     'gProbeListen = llListen(PROBE_CHANNEL, "", NULL_KEY, "");'),
    ('a call of anyone taken', 'if (speaker != gOwner || !onHud() ||', 'if (!onHud() ||'),
    ('a call of any text taken', ' || text != PROBE_TEXT + "|" + (string)llGetKey())', ')'),
    ('a call without the key taken (a dialog button)', ' || text != PROBE_TEXT + "|" + (string)llGetKey())', ' || text != PROBE_TEXT)'),
    ('a call with the owner\'s key taken', 'text != PROBE_TEXT + "|" + (string)llGetKey())', 'text != PROBE_TEXT + "|" + (string)llGetOwner())'),
    ('a call taken while not worn', 'if (speaker != gOwner || !onHud() ||', 'if (speaker != gOwner ||'),
    ('a call taken on the body', '    return (point >= 31 && point <= 38);', '    return (point >= 1);'),
    ('a call taken on the body (2)', '    return (point >= 31 && point <= 38);', '    return (point >= 31);'),
    ('a call taken on a point past the HUD', '    return (point >= 31 && point <= 38);', '    return (point >= 31 && point <= 99);'),
    ('a call taken on the body (3)', 'if (speaker != gOwner || !onHud() ||', 'if (speaker != gOwner || !llGetAttached() ||'),
    ('the call deadline set again by every URL request', '    gUrlRequest = llRequestSecureURL();\n    armTimer();',
     '    gUrlRequest = llRequestSecureURL();\n    gPairBy = llGetUnixTime() + PAIR_WAIT;\n    armTimer();'),
    ('announced to a viewer that did not call', '    if (!gPaired || !onHud())\n    {\n        return;\n    }\n    llOwnerSay("<charisBridge>"',
     '    llOwnerSay("<charisBridge>"'),
    ('announced from the body', '    if (!gPaired || !onHud())\n    {\n        return;\n    }\n    llOwnerSay("<charisBridge>"',
     '    if (!gPaired)\n    {\n        return;\n    }\n    llOwnerSay("<charisBridge>"'),
    ('the no-URL line said to a viewer that did not call', '    if (gPaired && onHud())\n    {\n        llOwnerSay("<charisBridgeNoUrl>"',
     '    if (TRUE)\n    {\n        llOwnerSay("<charisBridgeNoUrl>"'),
    ('the no-URL line said from the body', '    if (gPaired && onHud())\n    {\n        llOwnerSay("<charisBridgeNoUrl>"',
     '    if (gPaired)\n    {\n        llOwnerSay("<charisBridgeNoUrl>"'),
    ('the call not forgotten when worn again', '    gPaired = FALSE;\n    gLastReply = 0;', '    gLastReply = 0;'),
    ('the call not forgotten when taken off', '    gConnected = FALSE;\n    gPaired = FALSE;\n    gAskers = [];', '    gConnected = FALSE;\n    gAskers = [];'),
    ('the call not forgotten at a new rez', '        gPaired = FALSE;    // and none called it\n', ''),
    ('the call listener kept when taken off', '    llListenRemove(gProbeListen);\n    gProbeListen = 0;\n', ''),
    ('the call listener added again at every wear', '    llListenRemove(gProbeListen);\n    gProbeListen = llListen(', '    gProbeListen = llListen('),
    ('calls answered without a gap', 'if (gLastReply != 0 && stamp - gLastReply < PROBE_GAP)', 'if (FALSE)'),
    ('the answer to a call counted', 'announce(FALSE);', 'announce(TRUE);'),
    ('no taking itself off without a call', '        if (!gPaired && llGetUnixTime() >= gPairBy)', '        if (FALSE)'),
    ('taking itself off at once without a call', '        if (!gPaired && llGetUnixTime() >= gPairBy)', '        if (!gPaired)'),
    ('no taking itself off after unanswered announcements',
     '        // The viewer called but never answered: the bridge takes itself off.\n        llRequestPermissions(gOwner, PERMISSION_ATTACH);',
     '        // The viewer called but never answered: the bridge takes itself off.'),
    ('the call wait too short for the viewer', 'integer PAIR_WAIT     = 60;', 'integer PAIR_WAIT     = 20;'),
    ('another call channel', 'integer PROBE_CHANNEL = 473520184;', 'integer PROBE_CHANNEL = 473520185;'),
    ('another call text', 'string  PROBE_TEXT    = "charisBridgeProbe|1";', 'string  PROBE_TEXT    = "charisBridgeProbe|2";'),
    ('a brace missing', '        reply(request, 405, "method");\n        }', '        reply(request, 405, "method");\n'),
    ('another channel', '-184735220;', '-184735221;'),
    ('another version', 'string  VERSION       = "1.2";', 'string  VERSION       = "1.3";'),
    ('inventory drops allowed', 'llAllowInventoryDrop(FALSE);', 'llAllowInventoryDrop(TRUE);'),
    ('a URL asked for on a region change while not worn',
     'if ((change & (CHANGED_REGION | CHANGED_TELEPORT | CHANGED_REGION_START)) && llGetAttached())',
     'if (change & (CHANGED_REGION | CHANGED_TELEPORT | CHANGED_REGION_START))'),
    ('the timer at work while not worn', '        llSetTimerEvent(0.0);\n        if (!llGetAttached())\n        {\n            return;\n        }\n',
     '        llSetTimerEvent(0.0);\n'),
]

# The same on the viewer's lslbridge.cpp (its hello and its wait for a new URL).
CPP_MUTANTS = [
    # -- sharing, reflect and push (research/374, 374b)
    ('a sharing message sent without the hello or a URL', 'return b.state == State::Running && b.helloOk && !b.url.empty();',
     'return b.state == State::Running;'),
    ('no limit on shares a second', 'if (window.size() < most)', 'if (true)'),
    ('a launch not noted in the window', '        (end ? bridge().endLaunches : bridge().shareLaunches).push_back(t);\n', ''),
    ('the ends counted with the shares in one window', 'std::vector<F64>& window = end ? b.endLaunches : b.shareLaunches;', 'std::vector<F64>& window = b.shareLaunches;'),
    ('the ends cleared not at a dropped URL', '        b.endLaunches.clear();\n', ''),
    ('a quiet between failures not a new run', ' || t - b.laneFailLast > LANE_FAIL_GAP_SECONDS)', ')'),
    ('a rate answer not ending the run of failures', '                laneAnswered();\n                LL_DEBUGS("PlusLSLBridge") << "A "', '                LL_DEBUGS("PlusLSLBridge") << "A "'),
    ('a refusal not ending the run of failures', '            if (lane)\n            {\n                laneAnswered();\n            }\n            if (lane && b.laneRefusalTold)', '            if (lane && b.laneRefusalTold)'),
    ('the refusal warning not renewed by a new URL', '                b.laneRefusalTold = false; // a new URL: a refusal of a sharing post is warned once again\n', ''),
    ('the 8 posts on their way not kept when flushing', '        if (lane.inFlight.size() >= MAX_LANE_POSTS)\n        {\n            scheduleFlush(kind, key, SHARE_SPACING_SECONDS);', '        if (false)\n        {\n            scheduleFlush(kind, key, SHARE_SPACING_SECONDS);'),
    ('the window of launches not sliding', '[t](F64 at) { return t - at >= SHARE_WINDOW_SECONDS || at > t; }', '[t](F64 at) { return false && t - at > at; }'),
    ('the spacing counted from the launch, not the end of the post', '        setKeyTime(lane, key, now());\n        scheduleFlush(kind, key, spacingLeft(lane, key));',
     '        scheduleFlush(kind, key, spacingLeft(lane, key));'),
    ('an end not held to the spacing', '        return std::max(spacingLeft(lane, key), shareRateLeft(end));', '        return end ? 0.0 : std::max(spacingLeft(lane, key), shareRateLeft(end));'),
    ('no limit on sharing posts on their way', '        if (lane.inFlight.size() >= MAX_LANE_POSTS)\n        {\n            return false;\n        }\n        launchLane(kind, key, body, end);',
     '        launchLane(kind, key, body, end);'),
    ('no limit on sharing posts waiting', 'if (lane.waiting.size() >= MAX_LANE_POSTS)', 'if (false)'),
    ('an end replaced by a share', 'if (wait.end && !end)', 'if (false)'),
    ('the newest share not kept while one is on its way', '                wait.body = body;\n                wait.end  = end;\n                return true;',
     '                return true;'),
    ('the share lane kept when the URL is dropped', '        b.shareLane.clear(); // what was on the way', '        // what was on the way'),
    ('the push lane kept when the URL is dropped', '        b.pushLane.clear();\n', ''),
    ('a result of an older URL frees a key', '        if (epoch != b.epoch)\n        {\n            return; // the lane was cleared with the URL it was for (dropUrl)\n        }\n',
     ''),
    ('sharing posts counted in the 16 of sendPose', '        if (kind != PostKind::Share && kind != PostKind::Push)\n        {\n            ++b.postsInFlight;',
     '        if (true)\n        {\n            ++b.postsInFlight;'),
    ('a sharing line of any object taken for the bridge\'s', '            if (b.cleanOf != chatter->getID())', '            if (false)'),
    ('a sharing line naming ourselves taken', '            if (peer == gAgentID)', '            if (false)'),
    ('a sharing line out of its form taken', '            if (!ok)\n            {\n                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: form"',
     '            if (false)\n            {\n                LL_DEBUGS("PlusLSLBridge") << "A sharing line of the bridge dropped: form"'),
    ('a sharing line passed on while Off', '            if (b.state == State::Off)\n            {\n                return true;\n            }\n            // Copies',
     '            // Copies'),
    ('a sharing line of an unconfirmed speaker shown', '                return true;\n            }\n            if (b.cleanOf != chatter->getID())',
     '                return false;\n            }\n            if (b.cleanOf != chatter->getID())'),
    ('a sharing line shown in the chat', 'return handleSharingLine(kind, text, chatter, from_bridge);', 'return false;'),
    ('no spacing between a key\'s posts', '        const bool   waits = busy || delay > 0.0;', '        const bool   waits = busy;'),
    ('a spacing shorter than the receiver\'s gap', 'SHARE_SPACING_SECONDS = 0.35;', 'SHARE_SPACING_SECONDS = 0.1;'),
    ('a waiting body flushed for another URL', '        if (!LLApp::isRunning() || epoch != b.epoch)\n        {\n            return;\n        }\n        SharingLane& lane = laneOf(kind);\n        const auto   wait',
     '        if (!LLApp::isRunning())\n        {\n            return;\n        }\n        SharingLane& lane = laneOf(kind);\n        const auto   wait'),
    ('a waiting body flushed while its key has a post on the way', '        if (std::find(lane.inFlight.begin(), lane.inFlight.end(), key) != lane.inFlight.end())\n        {\n            return;\n        }\n        if (lane.inFlight.size() >= MAX_LANE_POSTS)',
     '        if (lane.inFlight.size() >= MAX_LANE_POSTS)'),
    ('a failed sharing post counted toward the lost URL', 'if (lane && result == PostResult::Transient)', 'if (false)'),
    ('the sharing URL never lost', '        if (t - b.laneFailSince >= LANE_FAIL_SECONDS)', '        if (false)'),
    ('the sharing URL lost at the first failure', '        if (t - b.laneFailSince >= LANE_FAIL_SECONDS)', '        if (true)'),
    ('an answer not ending the failing run', '            LL_INFOS("PlusLSLBridge") << "Sharing posts are answered again" << LL_ENDL;\n            b.laneFailSince = 0.0;',
     '            LL_INFOS("PlusLSLBridge") << "Sharing posts are answered again" << LL_ENDL;'),
    ('a rate refusal taken as any refusal', '        if (code == 429)', '        if (false)'),
    ('a rate refusal told as a warning', 'LL_DEBUGS("PlusLSLBridge") << "A " << postName(kind) << " was refused for its rate (429)"', 'LL_WARNS("PlusLSLBridge") << "A " << postName(kind) << " was refused for its rate (429)"'),
    ('a sharing refusal always warned', '            if (lane && b.laneRefusalTold)', '            if (false)'),
    ('a push to ourselves queued', 'if (target.isNull() || target == gAgentID || !laneReady())', 'if (target.isNull() || !laneReady())'),
    ('a share end queued as a share', 'proto::shareEndBody(bridge().token.asString(), seq), true);', 'proto::shareEndBody(bridge().token.asString(), seq), false);'),
    ('the result of a reflect told to the pose handler', 'reflect ? bridge().reflectResultHandler : bridge().sendResultHandler',
     'bridge().sendResultHandler'),
    ('a reflect said as a pose', '? proto::reflectBody(', '? proto::poseBody('),
    ('calls without a gap', 'now() < b.nextProbeAt || LLStartUp::getStartupState()', 'LLStartUp::getStartupState()'),
    ('a call although the URL is held', '|| !b.url.empty() || b.probeCount >= MAX_PROBES ||', '|| b.probeCount >= MAX_PROBES ||'),
    ('calls without an end', '!b.url.empty() || b.probeCount >= MAX_PROBES ||', '!b.url.empty() ||'),
    ('calls in the take-off', 'b.step == Step::None || b.step == Step::Detach ||', 'b.step == Step::None ||'),
    ('calls with no step', 'b.step == Step::None || b.step == Step::Detach ||', 'b.step == Step::Detach ||'),
    ('a call without the object\'s key', 'sendProbe(object->getID());', 'sendProbe(LLUUID::null);'),
    ('a recorded old bridge not waited for while its wear is on the way', 'if (LLAppearanceMgr::instance().isLinkedInCOF(recorded))\n            {\n                // Its wear', 'if (false)\n            {\n                // Its wear'),
    ('a recorded old bridge waited for only the step\'s time', '                    b.stepDeadline = now() + WEAR_SECONDS;\n                }\n                if (!stepExpired())', '                }\n                if (!stepExpired())'),
    ('a recorded old bridge not watched after it is forgotten', '            b.goneOld.push_back({ recorded, now() + 2 * WEAR_SECONDS });\n', ''),
    ('an old bridge that comes late kept', '        const bool gone_old = watchGoneOld();\n', '        const bool gone_old = false;\n'),
    ('an old bridge that comes late not taken off', '                LLVOAvatarSelf::detachAttachmentIntoInventory(it->id);\n', ''),
    ('the foreign bridges noted at every wait', '        if (found.notNull() && isOldBridgeName(it->getName()))', '        if (found.isNull()) { noteForeignBridges(*static_cast<LLInventoryModel::item_array_t*>(nullptr), found); }\n        if (found.notNull() && isOldBridgeName(it->getName()))'),
    ('a bridge on the body called', 'return (object && object->isHUDAttachment()) ? object : nullptr;', 'return object;'),
    ('a bridge on the body goes on to its contents after its repair', 'repair(Repair::Rewear, "worn on the body");\n                return;', 'repair(Repair::Rewear, "worn on the body");'),
    ('the wait for an old bridge never limited', '                    b.oldWaiting   = true;\n', ''),
    ('the old bridge\'s wait kept across steps', '        b.oldWaiting   = false;\n', ''),
    ('a late old bridge\'s COF link left', '                LLAppearanceMgr::instance().removeCOFItemLinks(it->id);\n', ''),
    ('a late old bridge watched for ever', 'detachAttachmentIntoInventory(it->id);\n                it = b.goneOld.erase(it);', 'detachAttachmentIntoInventory(it->id);\n                ++it;'),
    ('a bridge on the body kept', '            if (!object->isHUDAttachment())\n            {\n                // Worn on the body', '            if (false)\n            {\n                // Worn on the body'),
    ('a recorded old bridge\'s COF link left', '            LLAppearanceMgr::instance().removeCOFItemLinks(recorded); // else', '            // else'),
    ('calls before the login is done', ' || LLStartUp::getStartupState() < STATE_STARTED', ''),
    ('calls only after the steps found the item', 'const LLUUID  item = b.item.notNull() ? b.item : recordedItem();', 'const LLUUID  item = b.item;'),
    ('the call through the RLV filter', '        gMessageSystem->newMessageFast(_PREHASH_ChatFromViewer);\n        gMessageSystem->nextBlockFast(_PREHASH_AgentData);\n        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);\n        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);\n        gMessageSystem->nextBlockFast(_PREHASH_ChatData);\n        gMessageSystem->addStringFast(_PREHASH_Message, proto::probeText(bridge_object.asString()));\n        gMessageSystem->addU8Fast(_PREHASH_Type, CHAT_TYPE_WHISPER);\n        gMessageSystem->addS32("Channel", proto::PROBE_CHANNEL);\n        gAgent.sendReliableMessage();\n',
     '        send_chat_from_viewer(proto::probeText(), CHAT_TYPE_WHISPER, proto::PROBE_CHANNEL);\n'),
    ('the call said aloud', 'addU8Fast(_PREHASH_Type, CHAT_TYPE_WHISPER);', 'addU8Fast(_PREHASH_Type, CHAT_TYPE_SHOUT);'),
    ('the call on channel 0', 'addS32("Channel", proto::PROBE_CHANNEL);', 'addS32("Channel", 0);'),
    ('the calls not counted', '        ++b.probeCount;\n', ''),
    ('the calls without a gap', '        b.nextProbeAt = now() + PROBE_GAP_SECONDS;\n', ''),
    ('the calls of a step not begun again', '        b.probeCount   = 0; // every step is a new wait', '        // every step is a new wait'),
    ('the calls not ended when the steps are left', '        b.probeCount   = 0;\n        b.nextProbeAt  = 0.0;\n    }\n\n    // Once a session', '    }\n\n    // Once a session'),
    ('the calls not made by the tick', '        probeIfDue();\n        watchWorn();', '        watchWorn();'),
    ('a bridge of an old version worn', 'if (found.notNull() && isOldBridgeName(it->getName()))', 'if (false)'),
    ('a worn bridge of an old version kept on', '                takeOffOurselves(OwnDetach::Recreate);\n                return;\n            }\n            if (LLAppearanceMgr',
     '                return;\n            }\n            if (LLAppearanceMgr'),
    ('the old script not tidied away', '        return name == bridgeName() || isOldBridgeName(name);', '        return name == bridgeName();'),
    ('the viewer says hello before the contents were seen (low 2)',
     'helloWhenClean(); // at once only for the same URL', 'sendHello(); // at once only for the same URL'),
    ('cleanOf set although another script was found', '        else if (seen.known && worn && !older)\n',
     '        if (seen.known && worn && !older)\n'),
    ('cleanOf set by the answer of an older request', '        else if (seen.known && worn && !older)\n',
     '        else if (seen.known && worn)\n'),
    ('no note of a request on the way', '        b.contentsOlder = object->isInventoryPending();\n', ''),
    ('an older answer not asked again', '            askContents(object, ContentsAsk::Again); // once more', '            // once more'),
    ('an older answer asked again and again', 'if (worn && older && b.contentsAsk == ContentsAsk::First)', 'if (worn && older)'),
    ('the contents ask again after every answer', 'if (b.step == Step::AwaitAnswer && b.cleanOf == object_id)',
     'if (b.step == Step::AwaitAnswer && seen.known)'),
    ('cleanOf kept at another URL', '            b.cleanOf.setNull(); // a new URL', '            // a new URL'),
    ('cleanOf kept at a new attempt', '        b.answerWaited = true;\n        b.cleanOf.setNull();\n', '        b.answerWaited = true;\n'),
    ('pose lines taken before the contents were seen', '        if (bridge().cleanOf != chatter->getID())\n',
     '        if (false)\n'),
    ('a bridge\'s pose line not passing shown in the chat',
     '"A pose line of the bridge dropped: " << refusal << LL_ENDL;\n                return true;',
     '"A pose line of the bridge dropped: " << refusal << LL_ENDL;\n                return false;'),
    ('a pose line of the bridge not seen shown in the chat', 'if (from_id.notNull() && from_id == bridge().cleanOf)',
     'if (false)'),
    ('the contents refusal taken', '            return "contents";', '            return nullptr;'),
    ('the form refusal taken', '            return "form";', '            return nullptr;'),
    ('the self refusal taken', '            return "self";', '            return nullptr;'),
    ('the cleanOf check turned off', 'if (bridge().cleanOf != chatter->getID())', 'if (bridge().cleanOf != chatter->getID() && false)'),
    ('the self check turned off', 'if (LLUUID(line.peer) == gAgentID)', 'if (LLUUID(line.peer) == gAgentID && false)'),
    ('the speaker drop without its null check', 'if (from_id.notNull() && from_id == bridge().cleanOf)',
     'if (from_id == bridge().cleanOf)'),
    ('pose lines passed on while Off', 'if (b.state != State::Off && b.poseHandler)', 'if (b.poseHandler)'),
    ('a line taken as the bridge\'s although not fully audible',
     'chatter && CHAT_AUDIBLE_FULLY == chat.mAudible && isOurBridge(chatter)', 'chatter && isOurBridge(chatter)'),
    ('another object\'s URL line shown in the chat',
     '"A bridge line of an object that is not the worn bridge: dropped unread" << LL_ENDL;\n            return true;',
     '"A bridge line of an object that is not the worn bridge: dropped unread" << LL_ENDL;\n            return false;'),
    ('any worn object taken as the bridge item', '            return worn_item == b.item;', '            return true;'),
    ('the contents taken as the worn object\'s by any object', '        const bool      worn   = object && object->getID() == object_id;\n',
     '        const bool      worn   = object != nullptr;\n'),
    ('CheckContents not watched', '                              b.step == Step::CheckContents || b.step == Step::AddScript',
     '                              b.step == Step::AddScript'),
    ('a take-off in CheckContents not a repair', 'const bool hello_waited = b.step == Step::CheckContents ||\n                                  (',
     'const bool hello_waited = (\n                                  ('),
    ('any chat type looked at', 'if (CHAT_TYPE_OWNER != chat.mChatType || CHAT_SOURCE_OBJECT', 'if (CHAT_SOURCE_OBJECT'),
    ('another owner\'s lines looked at', ' || owner_id != gAgentID)\n        {\n            return false;', ')\n        {\n            return false;'),
    ('the bridge taken without its record', '        if (worn_item != recordedItem())\n', '        if (false)\n'),
    ('the bridge taken with linked parts', 'if (!root || root != chatter || !root->isAttachment() || root->numChildren() > 0 || ',
     'if (!root || root != chatter || !root->isAttachment() || '),
    ('the copied bridge not recorded', '        recordItem(new_item); // the only object', '        // the only object'),
    ('the record kept when the bridge is made anew', '            recordItem(LLUUID::null); // a new one is made', '            // a new one is made'),
    ('the record not checked', ' || !LLUUID::validate(text) || ', ' || '),
    ('FindItem without the record', '                                 ? recorded\n', '                                 ? (it ? it->getUUID() : recorded)\n'),
    ('linked parts allowed', '        if (object->numChildren() <= 0)\n', '        if (true)\n'),
    ('a bridge with linked parts kept on when no repair is left',
     '            LLVOAvatarSelf::detachAttachmentIntoInventory(bridge().item); // no repair left', '            // no repair left'),
    ('linked parts not looked at when worn', '            if (refuseLinkedParts(object))\n            {\n                return;\n            }\n            setStep(Step::CheckContents',
     '            setStep(Step::CheckContents'),
    ('linked parts not looked at before the hello', '        if (refuseLinkedParts(object))\n        {\n            return; // prims linked',
     '        if (false)\n        {\n            return; // prims linked'),
    ('the host not checked', '            options->setSSLVerifyHost(true);\n', ''),
    ('the certificate not checked', '            options->setSSLVerifyPeer(true);\n', ''),
    ('the send result not told', '                    handler(sent.action, sent.nonce, sent.target, ok);\n', ''),
    ('the send result false for a URL dropped since', 'const bool ok = result == PostResult::Ok;',
     'const bool ok = result == PostResult::Ok && epoch == b.epoch && b.state != State::Off;'),
    ('the send result told only when ok', '        if (kind == PostKind::Pose || kind == PostKind::Reflect)\n        {\n            const bool ok',
     '        if ((kind == PostKind::Pose || kind == PostKind::Reflect) && result == PostResult::Ok)\n        {\n            const bool ok'),
    ('the send result told from inside sendPose', '            doOnIdleOneTime([sent, ok, reflect]()\n            {\n',
     '            ([sent, ok, reflect]()\n            {\n'),
    ('the send result told while quitting', '                if (LLApp::isRunning() && handler)', '                if (handler)'),
    ('a certificate failure taken as transient', '                return PostResult::Certificate;', '                return PostResult::Transient;'),
    ('a certificate failure repaired', '            fail(Failure::Make, "certificate");\n', ''),
    ('a certificate failure not marked', '            b.certificateFailed = true; // the script says', '            // the script says'),
    ('announcements taken again at once after a certificate failure', '        if (b.state == State::Failed && b.certificateFailed)\n',
     '        if (false)\n'),
    ('the certificate mark kept across a region change', '        b.certificateFailed = false; // another simulator', '        // another simulator'),
    ('the certificate mark kept at a new attempt', '        b.urlRefused   = false;\n        b.certificateFailed = false;\n',
     '        b.urlRefused   = false;\n'),
    ('the record not saved at once', '            plus::saveSettingsFile(gSavedPerAccountSettings, gSavedSettings.getString("PerAccountSettingsFile"), true);\n        }\n    }\n\n    const std::string& bridgeName',
     '        }\n    }\n\n    const std::string& bridgeName'),
    ('a recorded bridge not read yet made a second time', '        if (recorded.notNull() && !it && !stepExpired())\n', '        if (false)\n'),
    ('the setting read for all accounts', 'LLCachedControl<bool> enabled(gSavedPerAccountSettings, SETTING_ENABLE', 'LLCachedControl<bool> enabled(gSavedSettings, SETTING_ENABLE'),
    ('scripts someone else made tidied', '                isOurName(it->getName()) && it->getCreatorUUID() == gAgentID)', '                isOurName(it->getName()))'),
    ('objects tidied again', 'if (!it->getIsLinkType() && it->getType() == LLAssetType::AT_LSL_TEXT && it->getUUID() != b.item &&',
     'if (!it->getIsLinkType() && it->getUUID() != b.item &&'),
    ('redirects followed', 'options->setFollowRedirects(false);', 'options->setFollowRedirects(true);'),
    ('a pose sent before the hello', 'if (b.state != State::Running || !b.helloOk || ', 'if (b.state != State::Running || '),
    ('our script known by its name only', ' && it->getCreatorUUID() == gAgentID)', ')'),
    ('the script tries not counted', '            if (++bridge().scriptTries > MAX_SCRIPT_TRIES)', '            if (false)'),
    ('the script tries counted at every making',
     '        setStep(Step::AddScript, SCRIPT_SECONDS);\n',
     '        if (++b.scriptTries > MAX_SCRIPT_TRIES)\n        {\n            fail(Failure::Make, "script item gone again");\n'
     '            return;\n        }\n        setStep(Step::AddScript, SCRIPT_SECONDS);\n'),
    ('the epoch not moved when the URL is dropped', '        b.laneFailLast  = 0.0;\n        ++b.epoch;\n', '        b.laneFailLast  = 0.0;\n'),
    ('takeOffWhileOff() takes nothing off',
     '            b.ownDetach = OwnDetach::Off;\n            LLVOAvatarSelf::detachAttachmentIntoInventory(b.item);\n        }\n    }\n\n    void onAnnouncement',
     '            b.ownDetach = OwnDetach::Off;\n        }\n    }\n\n    void onAnnouncement'),
    ('leaving the steps keeps the URL', '        Bridge& b = bridge();\n        dropUrl();\n        cancelContents();\n        b.step         = Step::None;',
     '        Bridge& b = bridge();\n        cancelContents();\n        b.step         = Step::None;'),
    ('an upload of another attempt taken',
     'if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::AddScript)\n        {\n            return;\n        }\n        if (!response',
     'if (!LLApp::isRunning() || b.step != Step::AddScript)\n        {\n            return;\n        }\n        if (!response'),
    ('a copy of another attempt taken', 'if (!LLApp::isRunning() || epoch != b.epoch || b.step != Step::CopyItem',
     'if (!LLApp::isRunning() || b.step != Step::CopyItem'),
    ('a take-off taken at once', 'constexpr F64 DETACH_SETTLE_SECONDS = 3.0;', 'constexpr F64 DETACH_SETTLE_SECONDS = 0.0;'),
    ('the script tries 200', 'constexpr int MAX_SCRIPT_TRIES      = 2;', 'constexpr int MAX_SCRIPT_TRIES      = 200;'),
    ('the refused announcements 200', 'constexpr int MAX_REFUSED_ANNOUNCEMENTS = 2;', 'constexpr int MAX_REFUSED_ANNOUNCEMENTS = 200;'),
    ('the first retry at once', 'constexpr F64 RETRY_FIRST_SECONDS   = 60.0;', 'constexpr F64 RETRY_FIRST_SECONDS   = 0.0;'),
    ('the signals kept when turned off', '        b.attachConnection.disconnect();\n        b.regionConnection.disconnect();\n', ''),
    ('Silent repaired where scripts do not run', '            if (!scriptsRunHere())\n', '            if (false)\n'),
    ('Silent repaired while the region gives no URL',
     'if (b.state == State::Silent && b.step == Step::None && !b.urlRefused)', 'if (b.state == State::Silent && b.step == Step::None)'),
    ('refused announcements never Failed', 'if (b.refusedInRow >= MAX_REFUSED_ANNOUNCEMENTS && settingOn()', 'if (false && settingOn()'),
    ('a hello sent again after its answer', 'if (!object || b.url.empty() || b.helloSent || b.helloOk)', 'if (!object || b.url.empty())'),
    ('another script version taken', 'if (announcement.version != proto::BRIDGE_VERSION)', 'if (false)'),
    ('a script file of any size', 'content.empty() || content.size() > SCRIPT_FILE_MAX_BYTES', 'content.empty()'),
    ('any other answer taken as ok', '        return PostResult::Refused;\n    }\n\n    // A pose or reflect message posted',
     '        return PostResult::Ok;\n    }\n\n    // A pose or reflect message posted'),
    ('the script tries kept across a long session', '                b.scriptTries  = 0; // ... and its script items', '                // ... and its script items'),
    ('a wear on its way kept when watchWorn sees it worn', '            b.wearAskedOf.setNull(); // worn: no wear', '            // worn: no wear'),
    ('a wear on its way kept when the bridge speaks', '        b.wearAskedOf.setNull(); // the worn bridge speaks', '        // the worn bridge speaks'),
    ('a wear on its way kept at the hello', '                b.wearAskedOf.setNull(); // it is worn and answering', '                // it is worn and answering'),
    ('the makings kept across a long session', '                b.scriptMakes  = 0; // made and running', '                // made and running'),
    ('a late wear marked as our take-off', '            LLVOAvatarSelf::detachAttachmentIntoInventory(b.wearAskedOf);\n',
     '            b.ownDetach = OwnDetach::Off;\n            LLVOAvatarSelf::detachAttachmentIntoInventory(b.wearAskedOf);\n'),
    ('foreign objects counted of any name', 'it->getType() == LLAssetType::AT_OBJECT &&\n                isOurName(it->getName()))',
     'it->getType() == LLAssetType::AT_OBJECT)'),
    ('onAnnouncement starts with the setting off',
     '        if (!settingOn())\n        {\n            takeOffWhileOff();\n            return;\n        }\n        if (b.state == State::Off)',
     '        if (b.state == State::Off)'),
    ('ensure() with the setting off',
     'if (!LLApp::isRunning() || !settingOn())\n        {\n            return;\n        }\n        if (bridge().state == State::Off)',
     'if (!LLApp::isRunning())\n        {\n            return;\n        }\n        if (bridge().state == State::Off)'),
    ('the login starts with the setting off', '        if (settingOn())\n        {\n            start();\n        }\n    }\n\n    plus::FeatureRegistration',
     '        {\n            start();\n        }\n    }\n\n    plus::FeatureRegistration'),
    ('a late wear watched while not stopped', '        if (!stopped)\n        {\n            return false; // the steps',
     '        if (false)\n        {\n            return false; // the steps'),
    ('a late wear watched without end', '        if (now() - b.wearAskedAt >= WEAR_SECONDS)\n', '        if (false)\n'),
    ('the tick running for ever while Off', '            b.ticking            = late_wear;\n            return !late_wear;',
     '            b.ticking            = true;\n            return false;'),
    ('a no-URL line taken while the setting is off',
     '        if (!settingOn())\n        {\n            takeOffWhileOff();\n            return;\n        }\n        LL_INFOS("PlusLSLBridge") << "Bridge has no URL',
     '        LL_INFOS("PlusLSLBridge") << "Bridge has no URL'),
    ('the setting turned off leaves the bridge on',
     '            b.ownDetach = OwnDetach::Off;\n            LLVOAvatarSelf::detachAttachmentIntoInventory(b.item);\n            if (b.wearAskedOf == b.item)',
     '            b.ownDetach = OwnDetach::Off;\n            if (b.wearAskedOf == b.item)'),
    ('anything judged as a script', 'if (!it || it->getType() != LLAssetType::AT_LSL_TEXT)', 'if (!it)'),
    ('scripts removed from an object not worn', 'if (!seen.extraScripts.empty() && worn)', 'if (!seen.extraScripts.empty())'),
    ('a library object copied without the copy permission', 'it->getPermissions().allowCopyBy(gAgentID) &&\n', ''),
    ('the makings limit 50', 'constexpr int MAX_SCRIPT_MAKES      = 5;', 'constexpr int MAX_SCRIPT_MAKES      = 50;'),
    ('the retries 40', 'constexpr int MAX_RETRIES           = 4;', 'constexpr int MAX_RETRIES           = 40;'),
    ('the repairs 30', 'constexpr int MAX_REPAIRS           = 3;', 'constexpr int MAX_REPAIRS           = 30;'),
    ('the take-off window 6 s', 'constexpr F64 REWEAR_WINDOW_SECONDS = 600.0;', 'constexpr F64 REWEAR_WINDOW_SECONDS = 6.0;'),
    ('the posts in flight without limit', 'target == gAgentID || b.postsInFlight >= MAX_POSTS_IN_FLIGHT)', 'target == gAgentID)'),
    ('a late post result taken', '        if (epoch == b.epoch && b.state != State::Off) // else',
     '        if (b.state != State::Off) // else'),
    ('any answer taken as ok', '== proto::REPLY_OK)', '!= "")'),
    ('Failed keeps a take-off of ours', '        leaveSteps();\n        bridge().ownDetach = OwnDetach::None;\n        setState(State::Failed);',
     '        leaveSteps();\n        setState(State::Failed);'),
    ('a new attempt keeps the wear on its way', '        b.wearAskedOf.setNull(); // a wear still on its way', '        // a wear still on its way'),
    ('an upload that finds the bridge off puts it on freely', '            onTakenOff(); // taken off meanwhile',
     '            setStep(Step::Wear, WEAR_SECONDS); // taken off meanwhile'),
    ('the makings not limited', '        if (++b.scriptMakes > MAX_SCRIPT_MAKES)\n', '        if (false)\n'),
    ('the makings counted again only by a new session', '        b.scriptMakes  = 0;\n', ''),
    ('items of any name tidied', '                isOurName(it->getName()) && it->getCreatorUUID() == gAgentID)', '                it->getCreatorUUID() == gAgentID)'),
    ('the bridge item tidied too', 'AT_LSL_TEXT && it->getUUID() != b.item &&\n', 'AT_LSL_TEXT &&\n'),
    ('links tidied too', 'if (!it->getIsLinkType() && it->getType() == LLAssetType::AT_LSL_TEXT', 'if (it->getType() == LLAssetType::AT_LSL_TEXT'),
    ('the guard not asked', '        if (plus::invguard::blocksItemRemoval(id))\n', '        if (false)\n'),
    ('items purged', '        gInventory.removeItem(id);', '        gInventory.purgeObject(id);'),
    ('names matched by their start', 'return name == bridgeName() || isOldBridgeName(name);', 'return LLStringUtil::startsWith(name, BRIDGE_NAME_PREFIX);'),
    ('a worn bridge item made anew trashed', 'gInventory.getItem(b.item) && !get_is_item_worn(b.item))', 'gInventory.getItem(b.item))'),
    ('a folder not loaded tidied', 'if (b.folder.isNull() || !gInventory.isCategoryComplete(b.folder))', 'if (b.folder.isNull())'),
    ('the bridge taken although it is not the item', '        if (worn_item != recordedItem())\n        {\n            return false;',
     '        if (worn_item != recordedItem())\n        {\n            return true;'),
    ('the script tries kept across attempts', '        b.scriptTries  = 0;\n        b.scriptMakes', '        b.scriptMakes'),
    ('the take-off turns never come free', 'if (at <= 0.0 || t - at >= REWEAR_WINDOW_SECONDS)', 'if (at <= 0.0)'),
    ('the take-off turns without end', '        if (!slot)\n        {\n            return false;', '        if (!slot)\n        {\n            return true;'),
    ('the repairs not counted', 'if (!b.repairs.take(MAX_REPAIRS))', 'if (false)'),
    ('the retries not counted', 'if (!b.retries.take(MAX_RETRIES))', 'if (false)'),
    ('a count forgiven without Running', 'if (used > 0 && ranSince && now()', 'if (used > 0 && now()'),
    ('ensure() starting from any state', '        if (bridge().state == State::Off)\n        {\n            start();',
     '        if (true)\n        {\n            start();'),
    ('a wear asked for not noted', '                b.wearAskedOf = b.item; // taken off again', '                // taken off again'),
    ('a late wear left on', '            LLVOAvatarSelf::detachAttachmentIntoInventory(b.wearAskedOf);\n', ''),
    ('a late wear not watched once Off', '        if (b.wearAskedOf.notNull())\n        {\n            wake();\n        }\n', ''),
    ('the script items of earlier makings left in the folder', '        removeOtherBridgeItems();\n        setStep(Step::AddScript',
     '        setStep(Step::AddScript'),
    ('the script added for an object no longer worn',
     '            if (!worn)\n            {\n                // Not the object worn now', '            if (false)\n            {\n                // Not the object worn now'),
    ('an answer left for the idle taken for a newer ask', ' || ask_number != b.contentsAskNumber)', ')'),
    ('the asks not numbered', '        ++b.contentsAskNumber;\n', ''),
    ('the one more look in CheckContents without its own time',
     '                b.stepDeadline = now() + STEP_SECONDS; // its own time', '                // its own time'),
    ('the viewer waits only 20 s after a region crossing', 'constexpr F64 ANSWER_SECONDS        = 30.0;', 'constexpr F64 ANSWER_SECONDS        = 20.0;'),
    ('the URL kept after another script was found',
     '            if (!b.url.empty() && !b.helloOk)\n            {\n                dropUrl();\n            }\n', ''),
    ('a child prim taken as the bridge', 'if (!root || root != chatter || !root->isAttachment()', 'if (!root || !root->isAttachment()'),
    ('the contents taken from the copy kept', '        object->dirtyInventory();\n', ''),
    # research/353 (on by default)
    ('the cached control off by default', 'SETTING_ENABLE, true); // as settings_plus.xml', 'SETTING_ENABLE, false); // as settings_plus.xml'),
    ('a safe start runs the bridge', 'return enabled && !plus::safemode::active();', 'return enabled;'),
    ('the setting turned on starts in a safe start',
     '        if (plus::safemode::active())\n        {\n            LL_INFOS("PlusLSLBridge") << "Bridge not started: a safe start" << LL_ENDL;\n            return;\n        }\n',
     ''),
    ('a change at the login taken as the person\'s',
     '        if (LLStartUp::getStartupState() == STATE_STARTED)\n        {\n            b.turnedOnByUser = true;',
     '        {\n            b.turnedOnByUser = true;'),
    ('the failures shown for a bridge that started by itself', '        if (!b.turnedOnByUser)\n', '        if (false)\n'),
    ('the failures never shown', '            b.turnedOnByUser = true; // the person', '            // the person'),
    ('the bridge made while the login\'s outfit is put on', 'gInventory.getRootFolderID().isNull() || outfitBeingPutOn())',
     'gInventory.getRootFolderID().isNull())'),
    ('the outfit never waited for', 'return gAgentWearables.changeInProgress();', 'return false;'),
    ('a wear asked for while an outfit is put on',
     '            if (outfitBeingPutOn())\n            {\n                b.stepDeadline = now() + WEAR_SECONDS;', '            if (false)\n            {\n                b.stepDeadline = now() + WEAR_SECONDS;'),
    ('a wear waited for an outfit without its time back', '                b.stepDeadline = now() + WEAR_SECONDS; // asked once the outfit is on',
     '                // asked once the outfit is on'),
    # research/353, the pre-merge review
    ('a Failed with a retry to come taken as for good', 'return b.state == State::Failed && b.retryAt <= 0.0 && !b.waitingForPlace;',
     'return b.state == State::Failed && !b.waitingForPlace;'),
    ('any item taken as the bridge by the outfit', 'return item_id.notNull() && item_id == recordedItem();', 'return item_id.notNull();'),
    ('the bridge kept in a new COF while it is not to run',
     'if (ours.isNull() || !settingOn() || !isAgentAvatarValid())\n', 'if (ours.isNull() || !isAgentAvatarValid())\n'),
    ('the bridge kept in a new COF when neither worn nor on its way',
     '        if (!gAgentAvatarp->getWornAttachment(ours) && !LLAppearanceMgr::instance().isLinkedInCOF(ours))\n', '        if (false)\n'),
    ('the new COF\'s list changed with the setting off', '        if (ours.isNull() || !settingOn())\n        {\n            return; // no bridge made',
     '        if (ours.isNull())\n        {\n            return; // no bridge made'),
    ('the bridge kept before the person\'s attachments', '        if (!bridgeFitsWith(other_objects))\n', '        if (false)\n'),
    ('the temporary attachments not counted', 'return other_objects + countTempAttachments() + 2 <= static_cast<U32>(most);',
     'return other_objects + 2 <= static_cast<U32>(most);'),
    ('the person\'s last place taken by the bridge', 'return other_objects + countTempAttachments() + 2 <= static_cast<U32>(most);',
     'return other_objects + countTempAttachments() + 1 <= static_cast<U32>(most);'),
    ('the bridge counted with the person\'s in a new COF', '            if (item.notNull() && item->getLinkedUUID() != ours)\n',
     '            if (item.notNull())\n'),
    ('a bridge link of the new outfit kept besides', '        dropBridgeLinks(obj_items);\n        if (LLViewerInventoryItem* item',
     '        if (LLViewerInventoryItem* item'),
    ('no place never tried again', '        if (b.state == State::Failed && b.waitingForPlace && roomForBridge())\n',
     '        if (false)\n'),
    ('no place tried again by the clock', '        bridge().waitingForPlace = failure == Failure::NoSlot;',
     '        bridge().waitingForPlace = false;'),
    ('a wait for a place kept through a new attempt', '        b.repairs.reset();\n        b.waitingForPlace = false;\n', '        b.repairs.reset();\n'),
    ('waiting for a place taken as failed for good', '&& b.retryAt <= 0.0 && !b.waitingForPlace;', '&& b.retryAt <= 0.0;'),
    ('the bridge\'s links left in the outfit\'s compare', '{ return item.notNull() && item->getLinkedUUID() == ours; }',
     '{ return false; }'),
    ('a wear asked for without a place', '            if (ask && !roomForBridge())\n', '            if (false)\n'),
    ('the last place taken', '!gAgentAvatarp->canAttachMoreObjects(2)', '!gAgentAvatarp->canAttachMoreObjects(1)'),
    ('the COF\'s attachments not counted', '        return bridgeFitsWith(listed);\n', '        return true;\n'),
    ('the temporary attachments not counted beside the COF\'s', '        return bridgeFitsWith(listed);\n',
     '        return listed + 2 <= static_cast<U32>(std::max(gAgentAvatarp->getMaxAttachments(), 0));\n'),
    ('the temporary attachments counted as none', '        return temporary;\n    }\n\n    // Whether the bridge has a place beside',
     '        return 0;\n    }\n\n    // Whether the bridge has a place beside'),
    ('the bridge counted against itself', 'listed += (link->getLinkedUUID() != item) ? 1 : 0;', 'listed += 1;'),
    ('no place tried again', 'fail(Failure::NoSlot, ', 'fail(Failure::Make, '),
    ('no place never told', 'case Failure::NoSlot:    LLNotificationsUtil::add("PlusLSLBridgeNoSlot"); break;', ''),
    ('a bridge that started by itself made anew for not answering',
     '(kind == Repair::Auto && b.repairs.used > 1 && b.turnedOnByUser);', '(kind == Repair::Auto && b.repairs.used > 1);'),
]

# Mutants of the other files the checks read (research/353, the pre-merge review): (path, what, old, new).
OTHER_MUTANTS = [
    (PROTO_CPP, 'lslbridgeproto.cpp: the key of the call not checked', 'if (!isUuid(objectKey))\n        {\n            return std::string();', 'if (false)\n        {\n            return std::string();'),
    (PROTO_CPP, 'lslbridgeproto.cpp: the key of the call not in lower case', "text.push_back((c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c);", 'text.push_back(c);'),
    (PROTO_CPP, 'lslbridgeproto.cpp: the call separated by a colon', "text.push_back('|');", "text.push_back(':');"),
    (APPEARANCE_CPP, 'llappearancemgr.cpp: the bridge saved into an outfit',
     '        if (plus::lslbridge::isBridgeItem(item->getLinkedUUID()))\n        {\n            continue;\n        }\n', ''),
    (APPEARANCE_CPP, 'llappearancemgr.cpp: the bridge taken off by a replace',
     '    plus::lslbridge::placeBridgeInNewCof(obj_items, append);\n', ''),
    (APPEARANCE_CPP, 'llappearancemgr.cpp: the bridge placed only when adding',
     '    plus::lslbridge::placeBridgeInNewCof(obj_items, append);\n', '    if (append) plus::lslbridge::placeBridgeInNewCof(obj_items, append);\n'),
    (APPEARANCE_CPP, 'llappearancemgr.cpp: the compare\'s exclusion written twice',
     '        plus::lslbridge::dropBridgeLinks(outfit_items);\n', '        plus::lslbridge::dropBridgeLinks(outfit_items);\n        plus::lslbridge::dropBridgeLinks(cof_items);\n'),
    (BACKUP_CPP, 'outfitbackup.cpp: the bridge kept in the copy',
     'if (item && item->getIsLinkType() && !plus::lslbridge::isBridgeItem(item->getLinkedUUID()))', 'if (item && item->getIsLinkType())'),
    (BACKUP_CPP, 'outfitbackup.cpp: the bridge\'s placement recorded',
     ' &&\n                    !plus::lslbridge::isBridgeItem(object->getAttachmentItemID())) // its placement is the bridge\'s own\n', ')\n'),
    (BACKUP_CPP, 'outfitbackup.cpp: an older copy\'s bridge put back',
     '            if (plus::lslbridge::isBridgeItem(link.id))\n            {\n                continue;', '            if (false)\n            {\n                continue;'),
    (BACKUP_CPP, 'outfitbackup.cpp: the worn bridge taken off by a restore',
     'if (LLViewerInventoryItem* bridge_item = plus::lslbridge::bridgeForOutfit(objects))', 'if (LLViewerInventoryItem* bridge_item = nullptr)'),
    (APPEARANCE_CPP, 'llappearancemgr.cpp: the worn bridge a change of the outfit',
     '        plus::lslbridge::dropBridgeLinks(cof_items);\n', ''),
    (APPEARANCE_CPP, 'llappearancemgr.cpp: an outfit saved with the bridge (an older viewer) always changed',
     '        plus::lslbridge::dropBridgeLinks(outfit_items);\n', ''),
    (POSER_CPP, 'fsfloaterposer.cpp: a bridge failed for good shown as not connected yet',
     'return plus::lslbridge::failedForGood() ? ConsentBlock::BridgeFailed : ConsentBlock::NoBridge;', 'return ConsentBlock::NoBridge;'),
    (POSER_XML['ja'], 'ja floater_fs_poser.xml: the failed bridge\'s line not translated',
     '\t<string name="consent_block_bridge_failed">', '\t<string name="consent_block_bridge_failed_x">'),
    (NOTIFICATIONS_XML['ja'], 'ja notifications.xml: the NoSlot notification missing', '<notification name="PlusLSLBridgeNoSlot">',
     '<notification name="PlusLSLBridgeNoSlotX">'),
]


def main():
    proto_h, proto_cpp, bridge_cpp = read(PROTO_H), read(PROTO_CPP), read(BRIDGE_CPP)
    if len(sys.argv) > 1 and sys.argv[1] == '--mutants':
        shipped = read(SHIPPED)
        # A mutant is caught by a failure the shipped script does not have (the shipped one should have none).
        baseline = set(run_checks(shipped, proto_h, proto_cpp, bridge_cpp))
        if baseline:
            print('the shipped script fails already (%d): the mutants are judged by the failures it does not have' % len(baseline))
        missed = 0
        for what, old, new in MUTANTS:
            if old not in shipped:
                print('MUTANT NOT APPLIED (its text is not in the script): %s' % what)
                missed += 1
                continue
            fails = [f for f in run_checks(shipped.replace(old, new, 1), proto_h, proto_cpp, bridge_cpp) if f not in baseline]
            print('%-70s %s' % (what, ('caught: ' + fails[0]) if fails else 'NOT CAUGHT'))
            missed += 0 if fails else 1
        for what, old, new in CPP_MUTANTS:
            if old not in bridge_cpp:
                print('MUTANT NOT APPLIED (its text is not in lslbridge.cpp): %s' % what)
                missed += 1
                continue
            fails = [f for f in run_checks(shipped, proto_h, proto_cpp, bridge_cpp.replace(old, new, 1)) if f not in baseline]
            print('%-70s %s' % ('lslbridge.cpp: ' + what, ('caught: ' + fails[0]) if fails else 'NOT CAUGHT'))
            missed += 0 if fails else 1
        for path, what, old, new in OTHER_MUTANTS:
            text = read(path)
            if old not in text:
                print('MUTANT NOT APPLIED (its text is not in %s): %s' % (os.path.basename(path), what))
                missed += 1
                continue
            OVERRIDES[path] = text.replace(old, new, 1)
            try:
                fails = [f for f in run_checks(shipped, read(PROTO_H), read(PROTO_CPP), read(BRIDGE_CPP)) if f not in baseline]
            finally:
                del OVERRIDES[path]
            print('%-70s %s' % (what, ('caught: ' + fails[0]) if fails else 'NOT CAUGHT'))
            missed += 0 if fails else 1
        total = len(MUTANTS) + len(CPP_MUTANTS) + len(OTHER_MUTANTS)
        print('%d of %d mutants not caught' % (missed, total) if missed else 'all %d mutants caught' % total)
        sys.exit(1 if missed else 0)
    path = sys.argv[1] if len(sys.argv) > 1 else SHIPPED
    fails = run_checks(read(path), proto_h, proto_cpp, bridge_cpp)
    for f in fails:
        print('FAIL:', f)
    if fails:
        print('%d check(s) failed' % len(fails))
        sys.exit(1)
    print('all checks passed')


if __name__ == '__main__':
    main()
