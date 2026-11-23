"""Normalised prototype and declaration hashes for the SD apps' export table (#519, tools/gen_export_table.py).

A prototype hash must change when an app built against the old header would break and stay when it would not: a
renamed parameter, `char* p` against `char *p`, `const char` against `char const`, `unsigned` against `unsigned int`
and a renamed function-pointer parameter are no API change. The types, structs, enums and macros a header declares are
hashed per header (declarations_hash), since a changed lichen_ctx_t or struct layout breaks apps without touching a
prototype."""
import hashlib
import os
import re

import check_layers as cl

QUALS = ("const", "volatile")
INT_MODS = frozenset(("unsigned", "signed", "short", "long"))
BASES = frozenset(("int", "char", "double", "float", "void", "bool", "_Bool"))
TAGS = ("struct", "enum", "union")
TOKEN_RE = re.compile(r"[A-Za-z_]\w*|\*|\[[^\]]*\]|\S")
FNPTR_RE = re.compile(r"^(.*?)\(\s*\*\s*(\w*)\s*\)\s*\((.*)\)\s*$", re.S)


def split_params(inner):
    out, depth, cur = [], 0, ""
    for ch in inner:
        depth += ch == "("
        depth -= ch == ")"
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    return out + [cur]


def canon_type(tokens):
    """Tokens of a type (no name) in one spelling: qualifiers after the base type, `unsigned` as `unsigned int`."""
    star = tokens.index("*") if "*" in tokens else len(tokens)
    head, tail = tokens[:star], tokens[star:]
    quals = sorted(t for t in head if t in QUALS)
    head = [t for t in head if t not in QUALS]
    if head and any(t in INT_MODS for t in head) and not any(t in BASES for t in head):
        head.append("int")
    return " ".join(head + quals + tail).replace("* *", "**")


def drop_name(tokens):
    """The tokens without the parameter's name: the last identifier, when something before it names the type."""
    if len(tokens) < 2:
        return tokens
    last = tokens[-1]
    arr = ""
    if last.startswith("["):
        arr, tokens = last, tokens[:-1]
        last = tokens[-1] if tokens else ""
    if re.match(r"^[A-Za-z_]\w*$", last) and last not in QUALS and last not in INT_MODS and last not in BASES:
        before = tokens[:-1]
        if before and before[-1] not in TAGS:
            return before + ([arr] if arr else [])
    return tokens + ([arr] if arr else [])


def normalise_param(p):
    """A parameter in one spelling and without its name."""
    p = " ".join(p.split())
    m = FNPTR_RE.match(p)
    if m:
        inner = ",".join(normalise_param(x) for x in split_params(m.group(3)))
        return "%s(*)(%s)" % (canon_type(TOKEN_RE.findall(m.group(1))), inner)
    return canon_type(drop_name(TOKEN_RE.findall(p)))


def prototype_hash(stmt):
    """Short hash of a declaration's normalised prototype: return type, name and parameter types."""
    stmt = " ".join(cl.ATTR_RE.sub(" ", stmt).split())
    i, j = stmt.index("("), stmt.rindex(")")
    head = TOKEN_RE.findall(stmt[:i])
    name = head.pop() if head else ""
    params = ",".join(normalise_param(p) for p in split_params(stmt[i + 1:j]))
    return hashlib.sha256(("%s %s(%s)" % (canon_type(head), name, params)).encode()).hexdigest()[:8]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def top_level_statements(text):
    """Statements of a header at brace depth 0, comments gone. A typedef, struct, enum or union runs to its `;`; a function
    body ends at its closing brace. #define lines (continuations joined) are statements too; other directives are not."""
    text = re.sub(r"\\\n", " ", strip_comments(text))
    text = re.sub(r"#\s*if(?:def)?\s*\(?\s*(?:defined\s*\(?\s*)?__cplusplus.*?#\s*endif", "", text, flags=re.S)  # extern "C"
    out, cur, depth = [], "", 0
    for line in text.splitlines():
        s = line.strip()
        if depth == 0 and not cur.strip() and s.startswith("#"):
            if re.match(r"#\s*define\b", s):
                out.append(s)
            continue
        for ch in s + " ":
            cur += ch
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth = max(depth - 1, 0)
                if depth == 0 and not re.match(r"\s*(typedef|struct|enum|union)\b", cur) and cur.rstrip().endswith("}"):
                    out.append(cur.strip())
                    cur = ""
            elif ch == ";" and depth == 0:
                out.append(cur.strip())
                cur = ""
    return out


def declarations_hash(text):
    """Hash of everything a header declares that is not a function: types, structs, enums, variables, #defines.
    Whitespace and comments do not count, the order does (a reordered enum is a different enum)."""
    keep = []
    for stmt in top_level_statements(text):
        is_type = re.match(r"(typedef|struct|enum|union)\b", stmt) is not None
        if stmt.startswith("#") or is_type and cl.statement_function(stmt) is None:
            keep.append(" ".join(stmt.split()))
        elif not is_type and "{" not in stmt and cl.statement_function(stmt) is None:
            keep.append(" ".join(stmt.split()))  # a variable
    return hashlib.sha256("\n".join(keep).encode()).hexdigest()[:8]


def local_includes(path):
    """The quoted #includes of a header that sit in the same directory."""
    d = os.path.dirname(path)
    text = cl.read(path)
    found = (m for m in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, flags=re.M))
    return [os.path.join(d, f) for f in found if os.path.exists(os.path.join(d, f))]


def header_closure(path, seen=None):
    """The header and, recursively, the headers it includes from its own directory, as {basename: path}."""
    seen = {} if seen is None else seen
    if os.path.basename(path) not in seen:
        seen[os.path.basename(path)] = path
        for inc in local_includes(path):
            header_closure(inc, seen)
    return seen
