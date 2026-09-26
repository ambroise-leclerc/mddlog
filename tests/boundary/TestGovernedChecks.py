"""Run each production check against deliberate violations; never accept an unrelated failure."""

import argparse
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
checker_path = Path(__file__).resolve().parents[2] / "scripts/check-governed.py"
spec = importlib.util.spec_from_file_location("governed", checker_path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    checker.arguments(parser)
    parser.add_argument("--bad-allocation", type=Path, required=True)
    parser.add_argument("--bad-exception", type=Path, required=True)
    args = parser.parse_args()
    original = checker.manifest(args.manifest)
    count = 0

    with tempfile.TemporaryDirectory(prefix="mddlog-governed-") as directory:
        temporary = Path(directory)

        def run(check, expected=None, data=None, root=None, probe=None):
            nonlocal count
            manifest_path = temporary / "manifest.txt"
            manifest_path.write_text("".join(f"{key}={';'.join(values)}\n" for key, values in
                                             (data if data is not None else original).items()), encoding="utf-8")
            command = [sys.executable, str(checker_path), check, "--manifest", str(manifest_path),
                       "--root", str(root or args.root), "--probe", str(probe or args.probe),
                       "--tool", args.tool, "--kind", args.kind]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            output = result.stdout + result.stderr
            if expected:
                checker.require(result.returncode == 1 and expected in output,
                                f"negative control did not fail for {expected}: {output}")
                print(f"detected {check} violation: {expected}")
            else:
                checker.require(result.returncode == 0, f"positive control failed: {output}")
            count += 1

        for check in ("graph", "source", "allocation", "exception"):
            run(check)

        for instrumentation in ("-fsanitize=thread", "-fsanitize=address,undefined"):
            data = copy.deepcopy(original)
            data["mddlog-core|LINK_OPTIONS"].append(instrumentation)
            run("graph", data=data)

        # Both P1689 versions are accepted, but an unknown revision remains fail-closed.
        source_object = Path(original["objects"][0])
        for version in (0, 2):
            data = copy.deepcopy(original)
            object_copy = temporary / f"version-{version}-{source_object.name}"
            shutil.copyfile(source_object, object_copy)
            ddi = json.loads(Path(str(source_object) + ".ddi").read_text(encoding="utf-8"))
            ddi["version"] = version
            ddi["rules"][0]["provides"][0]["source-path"] = original["sources"][0]
            Path(str(object_copy) + ".ddi").write_text(json.dumps(ddi), encoding="utf-8")
            data["objects"][0] = str(object_copy)
            run("graph", data=data) if version == 0 else run("graph", "unsupported P1689 scan", data=data)

        for key, edge in (("mddlog-core|LINK_LIBRARIES", "mddlog"),
                          ("mddlog-core|INTERFACE_LINK_LIBRARIES", "foreign-library"),
                          ("mddlog_options|INTERFACE_LINK_LIBRARIES", "mddlog::mddlog"),
                          ("mddlog_options|INTERFACE_LINK_LIBRARIES_DIRECT", "hidden-target"),
                          ("mddlog_warnings|INTERFACE_LINK_OPTIONS", "-lhidden")):
            data = copy.deepcopy(original)
            data[key].append(edge)
            run("graph", "unreviewed link dependency/option", data=data)
        data = copy.deepcopy(original)
        data["extra_sources"].append("/tmp/foreign-module.obj")
        run("graph", "unreviewed extra source", data=data)

        for imported in ("mddlog.sinks.sink", "mddlog.adapter.logrecord", "unexpected.module", "mddlog.core.ring"):
            data = copy.deepcopy(original)
            source_object = Path(data["objects"][0])
            object_copy = temporary / source_object.name
            shutil.copyfile(source_object, object_copy)
            ddi = json.loads(Path(str(source_object) + ".ddi").read_text(encoding="utf-8"))
            ddi["rules"][0]["provides"][0]["source-path"] = original["sources"][0]
            ddi["rules"][0].setdefault("requires", []).append({"logical-name": imported})
            Path(str(object_copy) + ".ddi").write_text(json.dumps(ddi), encoding="utf-8")
            data["objects"][0] = str(object_copy)
            run("graph", "import graph changed", data=data)

        for token in sorted(checker.FORBIDDEN):
            checker.require(checker.source_violations(f"void test() {{ {token}; }}"), f"source token escaped: {token}")
        for text in ("th\\\nrow 1;", "std /* comment */ :: vector<int> values;", "#include <vector>",
                     "std::chrono::system_clock::now();", "int a = 1'000; throw 1; int b = 2'000;"):
            checker.require(checker.source_violations(text), f"source construct escaped: {text}")
        for text in ('// throw new\nstd::string_view value;', '/* std::mutex */ "new throw";',
                     'auto value = R"tag(throw ") catch)tag";', "char value = 'x';", "Value(const Value&) = delete;"):
            checker.require(not checker.source_violations(text), f"source false positive: {text}")
        root = temporary / "source"
        shutil.copytree(args.root / "include/mddlog/core", root / "include/mddlog/core")
        (root / "include/mddlog/core/Injected.cppm").write_text("void bad() { throw 1; }\n", encoding="utf-8")
        run("source", "forbidden governed constructs", root=root)

        run("allocation", "allocation symbol violation", probe=args.bad_allocation)
        run("exception", "exception symbol violation", probe=args.bad_exception)
        # An allocation reference alone must not fail the distinct exception profile.
        run("exception", probe=args.bad_allocation)
        data = copy.deepcopy(original)
        data["objects"] = []
        run("allocation", "empty governed object inventory", data=data)
        empty = temporary / "empty.txt"
        empty.write_text("", encoding="utf-8")
        run("allocation", "empty/invalid object inventory", probe=empty)
        missing = temporary / "missing.txt"
        missing.write_text(str(temporary / "absent.o") + "\n", encoding="utf-8")
        run("exception", "missing/empty object", probe=missing)
    print(f"negative controls: {count} process checks plus {len(checker.FORBIDDEN)} token injections verified")


if __name__ == "__main__":
    try:
        main()
    except (checker.Violation, OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
