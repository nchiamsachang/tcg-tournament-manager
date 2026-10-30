"""checks run_tests.bat itself: runs the real script in scratch folders, with stub vsenv.bat and
build.bat and a fake test program (fake_qtest.cpp), and checks its output, exit codes and result
files.  run it through run_runner_tests.bat, which builds the fake program first.

usage: py -3 -I test_run_tests.py <fake_qtest.exe> <scratch folder>
"""
import os
import shutil
import subprocess
import sys

RUNNER = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "run_tests.bat")
FAKE = ""
ROOT = ""

VSENV = '@echo off\r\nif "%BUILD_DIR%"=="" set "BUILD_DIR=build"\r\nexit /b 0\r\n'
BUILD = ('@echo off\r\nsetlocal\r\ncall "%~dp0vsenv.bat"\r\n'
         '>>"%~dp0build_calls.txt" echo [%*]\r\n'
         'if defined FAKE_BUILD_FAIL exit /b 1\r\n'
         'if not exist "%~dp0%BUILD_DIR%" mkdir "%~dp0%BUILD_DIR%"\r\n'
         '>"%~dp0%BUILD_DIR%\\build_id.txt" echo fake-build-1\r\n'
         'exit /b 0\r\n')

results = []


def fixture(name, build_dir="build", missing=()):
    d = os.path.join(ROOT, name)
    shutil.rmtree(d, ignore_errors=True)
    out = os.path.join(d, build_dir)
    os.makedirs(os.path.join(out, "src", "ui"))
    shutil.copy(RUNNER, d)
    open(os.path.join(d, "vsenv.bat"), "w", newline="").write(VSENV)
    open(os.path.join(d, "build.bat"), "w", newline="").write(BUILD)
    for suite, rel in (("core", "test_core.exe"), ("rules", "test_rules.exe"), ("ui", r"src\ui\test_ui.exe")):
        if suite not in missing:
            shutil.copy(FAKE, os.path.join(out, rel))
    os.makedirs(os.path.join(d, "data"))
    return d, out


def run(d, args, extra=None, absolute=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith("FAKE_") and k != "BUILD_DIR"}
    env.update({"TCG_DATA_DIR": os.path.join(d, "data"), "FAKE_LOG": os.path.join(d, "starts.txt"),
                "NoDefaultCurrentDirectoryInExePath": "1"})
    env.update(extra or {})
    bat = os.path.join(d, "run_tests.bat") if absolute else r".\run_tests.bat"
    if any(c in a for a in args for c in "&;,="):
        # quoted, so the runner itself sees the character rather than cmd splitting the line
        line = "cmd /c %s %s" % (bat, " ".join('"%s"' % a for a in args))
        p = subprocess.run(line, cwd=d, env=env, capture_output=True, text=True)
    else:
        p = subprocess.run(["cmd", "/c", bat] + args, cwd=d, env=env, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def read(path):
    try:
        return open(path).read()
    except OSError:
        return ""


def check(case, cond, detail=""):
    results.append((case, cond))
    print("%s  %s%s" % ("PASS" if cond else "FAIL", case, ("  -- " + detail) if (detail and not cond) else ""))


def main():
    global FAKE, ROOT
    if len(sys.argv) != 3 or not os.path.isfile(sys.argv[1]):
        print(__doc__)
        return 2
    FAKE = os.path.abspath(sys.argv[1])
    ROOT = os.path.join(os.path.abspath(sys.argv[2]), "fixtures")
    base = "proj with spaces"

    # full run, default behavior
    d, out = fixture(base)
    rc, o = run(d, [])
    check("full: exit 0", rc == 0, o)
    check("full: says all three passed", "All three test programs passed." in o, o)
    check("full: builds everything (no --target)", read(os.path.join(d, "build_calls.txt")).strip() == "[]")
    starts = read(os.path.join(d, "starts.txt")).splitlines()
    check("full: started core, rules, ui once each with no selection",
          [s.split(" |")[0] for s in starts] == ["CoreTests", "RulesTests", "UiTests"], repr(starts))
    check("full: all three result files fresh", all(os.path.exists(os.path.join(out, s + "_results.txt"))
                                                     for s in ("core", "rules", "ui")))
    check("full: data folder passed through", all("data=" + os.path.join(d, "data") in s for s in starts))

    # full run with one failing program
    d, out = fixture(base)
    rc, o = run(d, [], {"FAKE_MODE": "fail"})
    check("full fail: exit 1", rc == 1, o)
    check("full fail: no pass message", "passed." not in o.split("FAILED")[-1] and "All three" not in o, o)

    # one suite
    d, out = fixture(base)
    for s in ("core", "rules", "ui"):
        open(os.path.join(out, s + "_results.txt"), "w").write("Totals: 999 passed, 0 failed\n")
    rc, o = run(d, ["RULES"])
    check("suite: exit 0", rc == 0, o)
    check("suite: builds only test_rules", read(os.path.join(d, "build_calls.txt")).strip() == "[--target test_rules]",
          read(os.path.join(d, "build_calls.txt")))
    check("suite: starts only rules", [s.split(" |")[0] for s in read(os.path.join(d, "starts.txt")).splitlines()]
          == ["RulesTests"])
    check("suite: prints requested suite", "Requested: suite rules, all of its functions" in o, o)
    check("suite: not reported as full", "All three" not in o and "This was not a full run. Not run: core ui." in o, o)
    check("suite: stale results of other suites removed",
          not os.path.exists(os.path.join(out, "core_results.txt")) and not os.path.exists(os.path.join(out, "ui_results.txt")))
    check("suite: stale 999 never shown", "999" not in o, o)

    # ui suite path
    d, out = fixture(base)
    rc, o = run(d, ["ui"])
    check("ui suite: exit 0 and runs src\\ui\\test_ui.exe", rc == 0 and r"src\ui\test_ui.exe" in o, o)

    # one function
    d, out = fixture(base)
    rc, o = run(d, ["core", "gamma"])
    starts = read(os.path.join(d, "starts.txt"))
    check("one function: exit 0", rc == 0, o)
    check("one function: program got exactly that selection", starts.startswith("CoreTests gamma |"), starts)
    check("one function: prints what ran", "test_core ran: PASS   : CoreTests::gamma()" in o, o)
    check("one function: debug lines not counted", "QDEBUG" not in o.split("Requested")[-1].split("ran:")[-1], o)
    check("one function: pass message names it", "Targeted run passed: suite core, functions gamma." in o, o)

    # several functions, data rows
    d, out = fixture(base)
    rc, o = run(d, ["core", "alpha", "beta"])
    check("two functions: exit 0", rc == 0, o)
    check("two functions: both reported", "CoreTests::alpha()" in o and "CoreTests::beta(x)" in o and "CoreTests::beta(y)" in o, o)
    check("two functions: not run listed", "Not run: rules ui." in o, o)

    # selected function fails
    d, out = fixture(base)
    rc, o = run(d, ["core", "beta"], {"FAKE_MODE": "fail"})
    check("failing function: exit 1", rc == 1, o)
    check("failing function: no pass message", "Targeted run passed" not in o, o)

    # selected function that only skips
    d, out = fixture(base)
    rc, o = run(d, ["core", "alpha"], {"FAKE_MODE": "skip"})
    check("skip only: exit 1", rc == 1, o)
    check("skip only: says it checked nothing", "test_core: alpha was only skipped, so it checked nothing." in o, o)
    check("skip only: no pass message", "Targeted run passed" not in o, o)

    # expected failure and blacklisted outcomes count as run
    d, out = fixture(base)
    rc, o = run(d, ["core", "alpha"], {"FAKE_MODE": "xfail"})
    check("xfail: exit 0 and reported", rc == 0 and "test_core ran: XFAIL  : CoreTests::alpha()" in o, o)
    rc, o = run(d, ["core", "alpha"], {"FAKE_MODE": "blacklist"})
    check("blacklisted: exit 0 and reported", rc == 0 and "test_core ran: BPASS  : CoreTests::alpha()" in o, o)

    # namespaced test class
    d, out = fixture(base)
    rc, o = run(d, ["rules", "gamma"], {"FAKE_NS": "1"})
    check("namespaced class: exit 0 and reported", rc == 0 and "ran: PASS   : app::RulesTests::gamma()" in o
          and "Targeted run passed" in o, o)

    # invalid suite and names
    d, out = fixture(base)
    for args in (["nosuch"], ["core", "-o"], ["core", "a-b"], ["core", "x;y"], ["core", "a b"], ["core", ""],
                 ["core", "1abc"], ["core", "a&b"], ["core", "a,b"], ["core", "a=b"], ["core", "alpha", "beta(x)"], ["core:alpha"]):
        rc, o = run(d, args)
        check("invalid %r: exit 2" % (args,), rc == 2, o)
    check("invalid: nothing built or started", not os.path.exists(os.path.join(d, "build_calls.txt"))
          and not os.path.exists(os.path.join(d, "starts.txt")))

    # unknown function
    d, out = fixture(base)
    rc, o = run(d, ["rules", "alpha", "nosuch"])
    check("unknown function: exit 1", rc == 1, o)
    check("unknown function: named", "test_rules: nosuch is not a test function of this suite." in o, o)
    check("unknown function: no pass message", "Targeted run passed" not in o, o)

    # program ignores the selection and exits 0
    d, out = fixture(base)
    rc, o = run(d, ["rules", "alpha"], {"FAKE_MODE": "ignoresel"})
    check("ignored selection: exit 1", rc == 1, o)
    check("ignored selection: says did not run", "test_rules: alpha did not run." in o, o)

    # missing executable, full and targeted
    d, out = fixture(base, missing=("rules",))
    rc, o = run(d, [])
    check("missing exe full: exit 1", rc == 1 and "does not exist" in o and "All three" not in o, o)
    rc, o = run(d, ["rules"])
    check("missing exe suite: exit 1", rc == 1 and "does not exist" in o and "passed" not in o, o)

    # missing results with a stale file present
    d, out = fixture(base)
    open(os.path.join(out, "core_results.txt"), "w").write("Totals: 999 passed, 0 failed\n")
    rc, o = run(d, ["core"], {"FAKE_MODE": "noresults"})
    check("no results: exit 1", rc == 1 and "wrote no results" in o, o)
    check("no results: stale 999 not shown", "999" not in o, o)
    rc, o = run(d, [], {"FAKE_MODE": "crash"})
    check("crash full: exit 1", rc == 1 and "wrote no results (exit code 3)" in o, o)

    # stale result file that cannot be deleted
    d, out = fixture(base)
    locked = os.path.join(out, "ui_results.txt")
    # a folder of that name cannot be removed by del
    os.makedirs(locked)
    open(os.path.join(locked, "x"), "w").write("x")
    rc, o = run(d, ["core"])
    check("undeletable old result: exit 1 before building", rc == 1 and "could not be removed" in o
          and not os.path.exists(os.path.join(d, "build_calls.txt")), o)

    # build failure
    d, out = fixture(base)
    rc, o = run(d, ["core"], {"FAKE_BUILD_FAIL": "1"})
    check("build failure: exit 1, nothing started", rc == 1 and not os.path.exists(os.path.join(d, "starts.txt")), o)

    # BUILD_DIR selection
    d, out = fixture(base, build_dir="build other")
    rc, o = run(d, ["rules", "alpha"], {"BUILD_DIR": "build other"})
    check("BUILD_DIR with spaces: exit 0", rc == 0 and os.path.exists(os.path.join(out, "rules_results.txt")), o)

    # help
    d, out = fixture(base)
    for args in (["help"], ["-h"], ["--help"], ["/?"]):
        rc, o = run(d, args)
        check("help %r: exit 0 with usage" % (args,), rc == 0 and "Usage: run_tests" in o, o)
    check("help: nothing built", not os.path.exists(os.path.join(d, "build_calls.txt")))

    # absolute quoted path and parentheses in the folder name
    d, out = fixture("proj (x86) with spaces")
    rc, o = run(d, ["core", "alpha"], absolute=True)
    check("parentheses + absolute path: exit 0", rc == 0 and "Targeted run passed" in o, o)
    rc, o = run(d, [], absolute=True)
    check("parentheses full: exit 0", rc == 0 and "All three test programs passed." in o, o)

    failed = [c for c, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
