"""LLDB data formatters for Edax programs (edaxc debug builds).

Loaded by the VS Code extension; for plain lldb:

    (lldb) command script import /path/to/edax_formatters.py

Shows Edax values the way the source writes them: strings as text, containers as their elements,
smart pointers as what they point at, and enums with payloads as the active alternative
(`some(42)`, `word("hi")`). edaxc's debug info describes the backend's layout; these formatters
know the standard library's private fields (library/std/std.ed).
"""

import lldb

CATEGORY = "edax"
MAX_STRING = 1024     # bytes shown of one string
MAX_CHILDREN = 1000   # elements shown of one container


# --- helpers ---------------------------------------------------------------------------------

def _uint(valobj, name):
    child = valobj.GetChildMemberWithName(name)
    return child.GetValueAsUnsigned(0) if child.IsValid() else 0


def _text(process, address, length):
    if address == 0 or length == 0:
        return '""'
    error = lldb.SBError()
    data = process.ReadMemory(address, min(length, MAX_STRING), error)
    if not error.Success():
        return "<unreadable>"
    text = data.decode("utf-8", "replace")
    text = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")
    return '"' + text + ('..."' if length > MAX_STRING else '"')


def _short(value):
    """One value on one line: its summary, its value, or its type."""
    if not value or not value.IsValid():
        return "?"
    summary = value.GetSummary()
    if summary:
        return summary
    plain = value.GetValue()
    if plain is not None:
        return plain
    return "{...}"


def _deref(valobj, field):
    """The value a pointer field points at, or None for a null pointer."""
    pointer = valobj.GetNonSyntheticValue().GetChildMemberWithName(field)
    if not pointer.IsValid() or pointer.GetValueAsUnsigned(0) == 0:
        return None
    return pointer.Dereference()


# --- strings -----------------------------------------------------------------------------------

def str_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    return _text(valobj.GetProcess(), _uint(v, "data"), _uint(v, "len"))


def string_summary(valobj, _dict):
    """std::string: 24 bytes (`repr`, three words). Inline up to 23 bytes, the length in byte 23;
    on the heap when the top bit of the third word is set: [data, length, capacity | 1 << 63]."""
    v = valobj.GetNonSyntheticValue()
    repr_ = v.GetChildMemberWithName("repr")
    words = repr_.GetChildMemberWithName("items") if repr_.GetNumChildren() == 1 else repr_
    w = [words.GetChildAtIndex(i).GetValueAsUnsigned(0) for i in range(3)]
    if w[2] >> 63:
        return _text(valobj.GetProcess(), w[0], w[1])
    length = (w[2] >> 56) & 0x7F
    return _text(valobj.GetProcess(), v.GetLoadAddress(), length)


def char_summary(valobj, _dict):
    """Edax's `char` is one Unicode scalar value (4 bytes)."""
    code = valobj.GetValueAsUnsigned(0)
    escapes = {0: "\\0", 9: "\\t", 10: "\\n", 13: "\\r", 39: "\\'", 92: "\\\\"}
    if code in escapes:
        return "'%s'" % escapes[code]
    if code < 32 or code > 0x10FFFF or 0xD800 <= code <= 0xDFFF:
        return "'\\u{%x}'" % code
    return "'%s'" % chr(code)


# --- contiguous containers: darray, span, array -------------------------------------------------

class ContiguousProvider:
    """`data` + `count`: std::darray<T>, std::span<T>."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.update()

    def update(self):
        v = self.valobj.GetNonSyntheticValue()
        self.data = v.GetChildMemberWithName("data")
        self.count = min(_uint(v, "count"), MAX_CHILDREN)
        self.element = self.data.GetType().GetPointeeType()
        self.size = self.element.GetByteSize() or 1
        return False

    def num_children(self):
        return self.count if self.data.GetValueAsUnsigned(0) else 0

    def get_child_index(self, name):
        try:
            return int(name.lstrip("[").rstrip("]"))
        except ValueError:
            return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.num_children():
            return None
        return self.data.CreateChildAtOffset("[%d]" % index, index * self.size, self.element)

    def has_children(self):
        return True


def contiguous_summary(valobj, _dict):
    return "size=%d" % _uint(valobj.GetNonSyntheticValue(), "count")


class ArrayProvider:
    """std::array<T, N>: its `items` field is a fixed array."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.update()

    def update(self):
        self.items = self.valobj.GetNonSyntheticValue().GetChildMemberWithName("items")
        return False

    def num_children(self):
        return min(self.items.GetNumChildren(), MAX_CHILDREN)

    def get_child_index(self, name):
        return self.items.GetIndexOfChildWithName(name)

    def get_child_at_index(self, index):
        return self.items.GetChildAtIndex(index)

    def has_children(self):
        return True


def array_summary(valobj, _dict):
    return "size=%d" % valobj.GetNonSyntheticValue().GetChildMemberWithName("items").GetNumChildren()


# --- smart pointers ------------------------------------------------------------------------------

class PointerProvider:
    """unique_ptr / shared_ptr / weak_ptr: the children of what `target` points at."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.update()

    def update(self):
        self.pointee = _deref(self.valobj, "target")
        return False

    def num_children(self):
        if self.pointee is None:
            return 0
        n = self.pointee.GetNumChildren()
        return n if n else 1

    def get_child_index(self, name):
        if self.pointee is None:
            return -1
        return self.pointee.GetIndexOfChildWithName(name)

    def get_child_at_index(self, index):
        if self.pointee is None:
            return None
        if self.pointee.GetNumChildren() == 0:
            return self.pointee.Clone("*target") if index == 0 else None
        return self.pointee.GetChildAtIndex(index)

    def has_children(self):
        return self.pointee is not None


def unique_ptr_summary(valobj, _dict):
    pointee = _deref(valobj, "target")
    if pointee is None:
        return "null"
    if pointee.GetNumChildren() == 0 or pointee.GetSummary():
        return _short(pointee)
    return "0x%x" % valobj.GetNonSyntheticValue().GetChildMemberWithName("target").GetValueAsUnsigned(0)


def shared_ptr_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    counts = v.GetChildMemberWithName("counts")
    if counts.GetValueAsUnsigned(0) == 0:
        return "null"
    c = counts.Dereference()
    strong = _uint(c.GetChildMemberWithName("strong"), "value")   # std::atomic<usize>
    weak = _uint(c.GetChildMemberWithName("weak"), "value") - (1 if strong else 0)   # the strong owners hold one
    pointee = _deref(valobj, "target")
    head = _short(pointee) + " " if pointee is not None and (pointee.GetNumChildren() == 0 or pointee.GetSummary()) else ""
    if strong == 0:
        return "expired"
    return "%sstrong=%d weak=%d" % (head, strong, weak)


# --- maps ---------------------------------------------------------------------------------------

class HashMapProvider:
    """std::hash_map<K, V>: a SwissTable. Slot i (a `{key, value}` hash_slot) is full when its
    control byte is below 0x80; there are mask + 1 slots."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.update()

    def update(self):
        v = self.valobj.GetNonSyntheticValue()
        self.entries = []
        ctrl = _uint(v, "ctrl")
        slots = v.GetChildMemberWithName("slots")
        if not ctrl or not slots.GetValueAsUnsigned(0):
            return False
        capacity = _uint(v, "mask") + 1
        error = lldb.SBError()
        flags = self.valobj.GetProcess().ReadMemory(ctrl, capacity, error)
        if not error.Success():
            return False
        stype = slots.GetType().GetPointeeType()
        for i in range(capacity):
            if flags[i] >= 0x80:
                continue
            slot = slots.CreateChildAtOffset("slot", i * stype.GetByteSize(), stype)
            key = slot.GetChildMemberWithName("key")
            self.entries.append((slot.GetChildMemberWithName("value"), "[%s]" % _short(key)))
            if len(self.entries) >= MAX_CHILDREN:
                break
        return False

    def num_children(self):
        return len(self.entries)

    def get_child_index(self, name):
        for i, (_v, label) in enumerate(self.entries):
            if label == name:
                return i
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= len(self.entries):
            return None
        value, label = self.entries[index]
        return value.Clone(label)

    def has_children(self):
        return True


class TreeMapProvider:
    """std::tree_map<K, V>: a B-tree of btree_node {len, leaf, keys, values, children}; the
    entries in key order (an in-order walk). Leaves have no children array."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.update()

    def _walk(self, pointer, depth):
        if depth > 40 or len(self.entries) >= MAX_CHILDREN or pointer.GetValueAsUnsigned(0) == 0:
            return
        node = pointer.Dereference()
        n = _uint(node, "len")
        leaf = node.GetChildMemberWithName("leaf").GetValueAsUnsigned(0) != 0
        keys = node.GetChildMemberWithName("keys")
        values = node.GetChildMemberWithName("values")
        children = None if leaf else node.GetChildMemberWithName("children")
        for i in range(n + 1):
            if children is not None:
                self._walk(children.GetChildAtIndex(i), depth + 1)
            if i < n and len(self.entries) < MAX_CHILDREN:
                self.entries.append((values.GetChildAtIndex(i), "[%s]" % _short(keys.GetChildAtIndex(i))))

    def update(self):
        self.entries = []
        self._walk(self.valobj.GetNonSyntheticValue().GetChildMemberWithName("root"), 0)
        return False

    def num_children(self):
        return len(self.entries)

    def get_child_index(self, name):
        for i, (_v, label) in enumerate(self.entries):
            if label == name:
                return i
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= len(self.entries):
            return None
        value, label = self.entries[index]
        return value.Clone(label)

    def has_children(self):
        return True


def map_summary(valobj, _dict):
    return "size=%d" % _uint(valobj.GetNonSyntheticValue(), "count")


def tree_map_summary(valobj, _dict):
    return "size=%d" % _uint(valobj.GetNonSyntheticValue(), "count")


def atomic_summary(valobj, _dict):
    return _short(valobj.GetNonSyntheticValue().GetChildMemberWithName("value"))


def mutex_summary(valobj, _dict):
    v = valobj.GetNonSyntheticValue()
    state = _uint(v, "state")
    return "%s %s" % ("unlocked" if state == 0 else "locked", _short(v.GetChildMemberWithName("data")))


# --- enums with payloads -------------------------------------------------------------------------

def is_payload_enum(sbtype, _dict):
    """edaxc lays out an enum with payloads as `struct { tag; union { ... } payload; }`."""
    t = sbtype.GetCanonicalType()
    if t.GetTypeClass() not in (lldb.eTypeClassStruct, lldb.eTypeClassClass) or t.GetNumberOfFields() != 2:
        return False
    tag = t.GetFieldAtIndex(0)
    payload = t.GetFieldAtIndex(1)
    return (tag.GetName() == "tag" and payload.GetName() == "payload"
            and tag.GetType().GetCanonicalType().GetTypeClass() == lldb.eTypeClassEnumeration
            and payload.GetType().GetCanonicalType().GetTypeClass() == lldb.eTypeClassUnion)


def _active(valobj):
    """(alternative name, payload value or None)."""
    v = valobj.GetNonSyntheticValue()
    tag = v.GetChildMemberWithName("tag").GetValue() or "?"
    payload = v.GetChildMemberWithName("payload").GetChildMemberWithName(tag)
    return tag, (payload if payload.IsValid() else None)


class PayloadEnumProvider:
    """The active alternative's payload as the only child."""

    def __init__(self, valobj, _dict):
        self.valobj = valobj
        self.update()

    def update(self):
        self.tag, self.payload = _active(self.valobj)
        return False

    def num_children(self):
        if self.payload is None:
            return 0
        n = self.payload.GetNumChildren()
        return n if n and not self.payload.GetSummary() else 1

    def get_child_index(self, name):
        if self.payload is None:
            return -1
        if self.num_children() == 1 and name == self.tag:
            return 0
        return self.payload.GetIndexOfChildWithName(name)

    def get_child_at_index(self, index):
        if self.payload is None:
            return None
        if self.num_children() == 1 and (self.payload.GetNumChildren() == 0 or self.payload.GetSummary()):
            return self.payload if index == 0 else None
        return self.payload.GetChildAtIndex(index)

    def has_children(self):
        return self.payload is not None


def payload_enum_summary(valobj, _dict):
    tag, payload = _active(valobj)
    if payload is None:
        return tag
    if payload.GetNumChildren() == 0 or payload.GetSummary():
        return "%s(%s)" % (tag, _short(payload))
    return "%s{...}" % tag


# --- registration --------------------------------------------------------------------------------

def __lldb_init_module(debugger, _dict):
    m = __name__
    commands = [
        # Edax names its integer types (u8, i8) like C chars: show numbers.
        "type format add -C no -f decimal -w {c} i8",
        "type format add -C no -f unsigned -w {c} u8",
        "type summary add -v -w {c} -F {m}.char_summary char32_t",
        "type summary add -w {c} -F {m}.str_summary std::str std::static_str",
        "type summary add -w {c} -F {m}.string_summary std::string",
        "type summary add -w {c} -F {m}.contiguous_summary -x '^std::(darray|span|span_mut)<.+>$'",
        "type synthetic add -w {c} -l {m}.ContiguousProvider -x '^std::(darray|span|span_mut)<.+>$'",
        "type summary add -w {c} -F {m}.atomic_summary -x '^std::atomic<.+>$'",
        "type summary add -w {c} -F {m}.mutex_summary -x '^std::mutex<.+>$'",
        "type summary add -w {c} -F {m}.array_summary -x '^std::array<.+>$'",
        "type synthetic add -w {c} -l {m}.ArrayProvider -x '^std::array<.+>$'",
        "type summary add -w {c} -F {m}.unique_ptr_summary -x '^std::unique_ptr<.+>$'",
        "type summary add -w {c} -F {m}.shared_ptr_summary -x '^std::(shared|weak)_ptr<.+>$'",
        "type synthetic add -w {c} -l {m}.PointerProvider -x '^std::(unique|shared|weak)_ptr<.+>$'",
        "type summary add -w {c} -F {m}.map_summary -x '^std::hash_map<.+>$'",
        "type synthetic add -w {c} -l {m}.HashMapProvider -x '^std::hash_map<.+>$'",
        "type summary add -w {c} -F {m}.tree_map_summary -x '^std::tree_map<.+>$'",
        "type synthetic add -w {c} -l {m}.TreeMapProvider -x '^std::tree_map<.+>$'",
        "type summary add -w {c} -F {m}.payload_enum_summary --recognizer-function {m}.is_payload_enum",
        "type synthetic add -w {c} -l {m}.PayloadEnumProvider --recognizer-function {m}.is_payload_enum",
        # Edax's std names (std::string, std::unique_ptr, ...) look like C++'s to lldb. A category
        # enabled later is searched first: libstdc++'s formatters never see Edax values.
        "type category enable {c}",
    ]
    for command in commands:
        debugger.HandleCommand(command.format(c=CATEGORY, m=m))
