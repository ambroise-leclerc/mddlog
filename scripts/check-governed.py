"""Independent, fail-closed ADR-001 boundary checks (Python standard library only).

Object scan design inspired by MduXNoHeapScan.cmake at d972d77bc5cefdbe105ad7933ee61746fb5eb45b.
Unlike its combined ml-noheap profile, allocation and exception symbols are separate here.
No runtime or whole-program guarantee follows from a successful scan; see docs/governed-evidence.md.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


class Violation(RuntimeError):
    """A checked input violates the policy, or evidence is missing."""


def require(condition, message):
    if not condition:
        raise Violation(message)


def manifest(path):
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        require(separator and key not in result, f"invalid manifest line: {line}")
        result[key] = [item for item in value.split(";") if item]
    return result


def check_links(data, policy):
    for target, allowed in policy["links"].items():
        for prop in ("LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES_DIRECT",
                     "INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE", "LINK_OPTIONS", "INTERFACE_LINK_OPTIONS"):
            key = f"{target}|{prop}"
            require(key in data, f"missing graph evidence: {key}")
            for edge in data[key]:
                if prop in ("INTERFACE_LINK_LIBRARIES_DIRECT", "INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE"):
                    valid = False  # No direct-link injection or removal is currently reviewed.
                elif prop.endswith("OPTIONS"):
                    # Existing sanitizer instrumentation is not an application dependency.
                    valid = target == "mddlog_options" and re.fullmatch(
                        r"-fsanitize=(address|leak|undefined|thread|memory)(,(address|leak|undefined|thread|memory))*", edge
                    )
                else:
                    valid = edge in allowed
                require(valid, f"unreviewed link dependency/option: {key} -> {edge}")


def check_module_rule(rule, policy):
    provides = rule.get("provides", [])
    require(len(provides) == 1, "governed object must provide exactly one reviewed module")
    name = provides[0]["logical-name"]
    require(name in policy["modules"], f"unreviewed module: {name}")
    imports = {item["logical-name"] for item in rule.get("requires", [])}
    expected = set(policy["modules"][name])
    require(imports == expected, f"import graph changed for {name}: expected {sorted(expected)}, got {sorted(imports)}")
    return name


def check_graph(root, data):
    policy = json.loads((root / "cmake/governed-policy.json").read_text(encoding="utf-8"))
    check_links(data, policy)
    sources = {(root / value).resolve() for value in data["sources"]}
    require(sources, "empty governed source set")
    for source in sources:
        require(source.is_relative_to((root / "include/mddlog/core").resolve()), f"source outside governed tree: {source}")
    for source in data["extra_sources"]:
        require((root / source).resolve() in sources, f"unreviewed extra source: {source}")
    objects = data["objects"]
    require(len(objects) == len(sources), "governed object/source inventory mismatch")
    seen = set()
    seen_sources = set()
    for obj in objects:
        require(Path(obj).is_file(), f"missing built object: {obj}")
        ddi = Path(obj + ".ddi")
        require(ddi.is_file(), f"missing compiler dependency scan: {ddi}")
        scan = json.loads(ddi.read_text(encoding="utf-8"))
        require(scan.get("version") == 1 and len(scan.get("rules", [])) == 1, f"unsupported P1689 scan: {ddi}")
        rule = scan["rules"][0]
        name = check_module_rule(rule, policy)
        require(name not in seen, f"duplicate module provider: {name}")
        seen.add(name)
        source = Path(rule["provides"][0]["source-path"]).resolve()
        require(source in sources, f"unreviewed module source: {source}")
        seen_sources.add(source)
    require(seen == set(policy["modules"]), "module inventory differs from reviewed policy")
    require(seen_sources == sources, "source inventory differs from compiler dependency scans")
    print(f"graph: {len(seen)} built modules; exact import edges and evaluated target link dependencies checked")


# Exact tokens, also after 'using namespace std': string_view is deliberately not string.
# No source suppressions are accepted. Comments and literals are not C++ operations.
FORBIDDEN = set("""
try catch throw new delete
string wstring u8string u16string u32string basic_string vector deque list forward_list map multimap
unordered_map unordered_multimap set multiset unordered_set unordered_multiset allocator polymorphic_allocator
make_shared allocate_shared make_unique shared_ptr unique_ptr function
mutex recursive_mutex timed_mutex recursive_timed_mutex shared_mutex shared_timed_mutex
condition_variable condition_variable_any lock_guard scoped_lock unique_lock shared_lock
thread jthread future promise async packaged_task semaphore counting_semaphore binary_semaphore latch barrier
stringstream ostringstream istringstream basic_stringstream wstringstream wostringstream wistringstream
iostream fstream ifstream ofstream cout cerr clog cin format vformat format_to vformat_to
malloc calloc realloc aligned_alloc posix_memalign free strdup strndup alloca
gmtime gmtime_r gmtime_s localtime localtime_r localtime_s mktime clock
sleep sleep_for sleep_until usleep nanosleep pthread_mutex_lock pthread_cond_wait
""".split())

TOKEN = re.compile(
    r'(?P<comment>//[^\n]*|/\*.*?\*/)'
    r'|(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^()\\\s]{0,16})\(.*?\)(?P=delimiter)")'
    r"|(?P<number>[0-9](?:[A-Za-z_0-9.]|'[A-Za-z_0-9])*)"
    r'|(?P<literal>(?:u8|u|U|L)?"(?:\\.|[^"\\\r\n])*"|(?:u8|u|U|L)?\'(?:\\.|[^\'\\\r\n])*\')'
    r'|(?P<identifier>[A-Za-z_][A-Za-z_0-9]*)|(?P<other>\S)', re.DOTALL
)


def source_violations(text):
    # C++ line splicing precedes tokenization: e.g. th\\\nrow must still be rejected.
    text = re.sub(r"\\\r?\n", "", text)
    tokens = [(match.group(), match.start()) for match in TOKEN.finditer(text)
              if match.lastgroup not in ("comment", "raw", "literal")]
    violations = []
    for index, (token, offset) in enumerate(tokens):
        forbidden = token in FORBIDDEN
        # '= delete;' declares an unavailable operation; it does not release heap storage.
        if token == "delete" and index and tokens[index - 1][0] == "=" and index + 1 < len(tokens) and tokens[index + 1][0] == ";":
            forbidden = False
        # Local includes/macros could conceal dependencies outside the scanned directory.
        forbidden |= token == "#"
        if token in ("now", "time") and index and tokens[index - 1][0] == ":":
            forbidden = True
        if forbidden:
            violations.append(f"line {text.count(chr(10), 0, offset) + 1}: {token}")
    return violations


def check_sources(root):
    files = sorted(path for path in (root / "include/mddlog/core").rglob("*") if path.is_file())
    require(files, "empty governed source tree")
    violations = []
    for path in files:
        violations.extend(f"{path}: {value}" for value in source_violations(path.read_text(encoding="utf-8")))
    require(not violations, "forbidden governed constructs:\n" + "\n".join(violations))
    print(f"source: {len(files)} files checked; no forbidden tokens (no suppression list)")


SYMBOLS = {
    "allocation": re.compile(
        r"operator (?:new|delete)\b|\?\?(?:2|3|_U|_V)@|_Zn[aw]|_Zd[al]Pv"
        r"|\b_*(?:malloc|calloc|realloc|free|aligned_alloc|posix_memalign|strdup|strndup)(?:\b|@)"
        r"|__cxa_allocate_exception"
    ),
    "exception": re.compile(r"__cxa_(?:throw|rethrow|bad_cast|bad_typeid)|__throw_"
                            r"|_CxxThrowException|_CxxThrowBadArrayNewLength|_X(?:bad_alloc|length_error|out_of_range|invalid_argument|runtime_error)")
}


def read_objects(path):
    values = Path(path).read_text(encoding="utf-8").splitlines()
    require(values and all(values), f"empty/invalid object inventory: {path}")
    return values


def scan_objects(objects, tool, kind, profile):
    require(objects, "empty object scan")
    violations = []
    for obj in objects:
        require(Path(obj).is_file() and Path(obj).stat().st_size, f"missing/empty object: {obj}")
        command = [tool, "/SYMBOLS", obj] if kind == "dumpbin" else [tool, "-u", "-C", obj]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", check=False)
        require(result.returncode == 0, f"symbol tool failed for {obj}: {result.stderr}")
        for line in result.stdout.splitlines():
            if kind == "dumpbin" and "UNDEF" not in line:
                continue
            if SYMBOLS[profile].search(line):
                violations.append(f"{obj}: {line.strip()}")
    require(not violations, f"{profile} symbol violation:\n" + "\n".join(violations))
    print(f"{profile}: {len(objects)} objects scanned with {tool}; no listed undefined references")


def arguments(parser):
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--kind", choices=("nm", "dumpbin"), required=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check", choices=("graph", "source", "allocation", "exception"))
    arguments(parser)
    args = parser.parse_args()
    data = manifest(args.manifest)
    if args.check == "graph":
        check_graph(args.root, data)
    elif args.check == "source":
        check_sources(args.root)
    else:
        require(data["objects"], "empty governed object inventory")
        scan_objects(data["objects"] + read_objects(args.probe), args.tool, args.kind, args.check)


if __name__ == "__main__":
    try:
        main()
    except (Violation, OSError, ValueError, KeyError) as error:
        print(f"governed check FAILED: {error}", file=sys.stderr)
        sys.exit(1)
