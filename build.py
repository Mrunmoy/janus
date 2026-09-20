#!/usr/bin/env python3
"""
build.py - Convenience Build, Test, Benchmark, and Quality Automation Script for libcfuture.

Supports Linux, macOS, and Windows environments. The reference toolchain is the Nix dev
shell (`nix develop -c python3 build.py --all`); the figures quoted in README.md come from it.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys

IS_WINDOWS = platform.system() == "Windows"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


class Colors:
    HEADER = "\033[95m" if not IS_WINDOWS else ""
    BLUE = "\033[94m" if not IS_WINDOWS else ""
    GREEN = "\033[92m" if not IS_WINDOWS else ""
    YELLOW = "\033[93m" if not IS_WINDOWS else ""
    RED = "\033[91m" if not IS_WINDOWS else ""
    BOLD = "\033[1m" if not IS_WINDOWS else ""
    RESET = "\033[0m" if not IS_WINDOWS else ""


def log_info(msg: str):
    print(f"{Colors.BLUE}[INFO]{Colors.RESET} {msg}")


def log_success(msg: str):
    print(f"{Colors.GREEN}[SUCCESS]{Colors.RESET} {msg}")


def log_warn(msg: str):
    print(f"{Colors.YELLOW}[WARN]{Colors.RESET} {msg}")


def log_error(msg: str):
    print(f"{Colors.RED}[ERROR]{Colors.RESET} {msg}")


def run_cmd(cmd, cwd=SCRIPT_DIR, check=True):
    log_info(f"Running: {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str))
    if check and res.returncode != 0:
        log_error(f"Command failed with exit code {res.returncode}")
        sys.exit(res.returncode)
    return res.returncode


def clean():
    log_info("Cleaning build artifacts...")
    dirs_to_clean = [
        "build",
        "build_nix",
        "build_tsan",
        "build_asan",
        "build_cov",
    ]
    for d in dirs_to_clean:
        path = os.path.join(SCRIPT_DIR, d)
        if os.path.exists(path):
            shutil.rmtree(path)
            log_info(f"Removed {d}/")
    log_success("Clean completed.")


def build_release(build_dir="build"):
    log_info(f"Configuring and building Release in {build_dir}/...")
    run_cmd(["cmake", "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"])
    run_cmd(["cmake", "--build", build_dir])
    log_success("Release build completed.")


def run_tests(build_dir="build"):
    log_info(f"Running test suite in {build_dir}/...")
    if not os.path.exists(os.path.join(SCRIPT_DIR, build_dir)):
        build_release(build_dir)
    run_cmd(["ctest", "--test-dir", build_dir, "--output-on-failure"])
    log_success("All tests passed.")


def run_tsan():
    log_info("Running ThreadSanitizer suite (100k concurrency cycles)...")
    build_dir = "build_tsan"
    run_cmd(["cmake", "-B", build_dir, "-DCMAKE_BUILD_TYPE=Debug", "-DCFUTURE_ENABLE_TSAN=ON"])
    run_cmd(["cmake", "--build", build_dir])
    run_cmd(["ctest", "--test-dir", build_dir, "--output-on-failure"])
    log_success("ThreadSanitizer suite passed with 0 warnings.")


def run_asan():
    log_info("Running AddressSanitizer and UndefinedBehaviorSanitizer suite...")
    build_dir = "build_asan"
    run_cmd(["cmake", "-B", build_dir, "-DCMAKE_BUILD_TYPE=Debug", "-DCFUTURE_ENABLE_ASAN=ON"])
    run_cmd(["cmake", "--build", build_dir])
    run_cmd(["ctest", "--test-dir", build_dir, "--output-on-failure"])
    log_success("AddressSanitizer and UBSan suite passed with 0 errors.")


def run_benchmarks(build_dir="build"):
    log_info("Running micro-benchmarks...")
    if not os.path.exists(os.path.join(SCRIPT_DIR, build_dir)):
        build_release(build_dir)
    bench_bin = os.path.join(SCRIPT_DIR, build_dir, "benchmarks", "bench_throughput")
    if IS_WINDOWS:
        bench_bin += ".exe"
    run_cmd([bench_bin])


def run_stats(build_dir="build"):
    log_info("Inspecting memory footprint and zero-heap verification...")
    lib_path = os.path.join(SCRIPT_DIR, build_dir, "libcfuture.a")
    if not os.path.exists(lib_path):
        build_release(build_dir)

    print(f"{Colors.BOLD}--- Binary Footprint (size) ---{Colors.RESET}")
    if shutil.which("size"):
        run_cmd(["size", lib_path], check=False)
    else:
        log_warn("'size' utility not found on PATH.")

    print(f"{Colors.BOLD}--- Zero-Heap Dynamic Allocation Check (nm) ---{Colors.RESET}")
    if shutil.which("nm"):
        proc = subprocess.run(
            f"nm {lib_path} | grep -E '(malloc|free|calloc|realloc)'",
            shell=True,
            capture_output=True,
            text=True,
        )
        if proc.stdout.strip():
            log_error(f"Found dynamic allocations:\n{proc.stdout}")
            sys.exit(1)
        else:
            log_success("0 dynamic allocation symbols found. Zero-heap verified!")
    else:
        log_warn("'nm' utility not found on PATH.")


def run_coverage():
    log_info("Generating code coverage report via lcov...")
    build_dir = "build_cov"
    run_cmd(["cmake", "-B", build_dir, "-DCMAKE_BUILD_TYPE=Debug", "-DCFUTURE_ENABLE_COVERAGE=ON"])
    run_cmd(["cmake", "--build", build_dir])
    run_cmd(["ctest", "--test-dir", build_dir])

    if shutil.which("lcov"):
        # lcov 2.x takes a tool plus its arguments as repeated --gcov-tool flags;
        # a single quoted "llvm-cov gcov" is looked up as one executable and fails.
        gcov_tool = (
            "--gcov-tool llvm-cov --gcov-tool gcov" if shutil.which("llvm-cov") else "--gcov-tool gcov"
        )
        cmd = (
            f"lcov {gcov_tool} --capture --directory {build_dir} "
            f"--output-file {build_dir}/coverage.info --ignore-errors unused,unsupported,version "
            f"--exclude '/nix/*' --exclude '*/tests/*' --exclude '*/benchmarks/*' --exclude '*/usr/*' "
            f"&& lcov --list {build_dir}/coverage.info"
        )
        run_cmd(cmd)
        log_success("Code coverage report generated.")
    else:
        log_warn("lcov not installed; skipping lcov report generation.")


def run_lint():
    log_info("Running static analysis and code formatting checks...")
    if shutil.which("cppcheck"):
        log_info("Running cppcheck...")
        run_cmd([
            "cppcheck",
            "--enable=all",
            "--suppress=missingIncludeSystem",
            "--suppress=unusedFunction",
            "--suppress=normalCheckLevelMaxBranches",
            "--error-exitcode=1",
            "-I",
            "include",
            "src/",
        ])
        log_success("Cppcheck passed.")
    else:
        log_warn("cppcheck not installed; skipping cppcheck.")

    if shutil.which("clang-format"):
        log_info("Running clang-format dry-run check...")
        run_cmd(
            "clang-format --dry-run --Werror src/*.c src/adapters/*.c include/*.h include/adapters/*.h tests/*.cpp tests/*.hpp benchmarks/*.cpp examples/*.c"
        )
        log_success("clang-format check passed.")
    else:
        log_warn("clang-format not installed; skipping format check.")


def run_docs_check(build_dir="build"):
    """Fails when README.md drifts from the code it documents."""
    import glob
    import re

    log_info("Checking README.md against the code...")
    readme = open(os.path.join(SCRIPT_DIR, "README.md"), encoding="utf-8").read()
    problems = []

    def read_all(patterns):
        text = ""
        for pattern in patterns:
            for path in glob.glob(os.path.join(SCRIPT_DIR, pattern), recursive=True):
                text += open(path, encoding="utf-8").read()
        return text

    code = read_all(["include/**/*.h", "src/**/*.c", "tests/*", "benchmarks/*", "examples/*"])
    api = read_all(["include/**/*.h"])

    # Every library identifier the README mentions must exist somewhere in the code.
    for ident in sorted(set(re.findall(r"\b(?:cfuture|cpromise|CFUTURE)_[A-Za-z0-9_]+", readme))):
        if ident != "cfuture_palh" and not re.search(r"\b" + re.escape(ident) + r"\b", code):
            problems.append(f"README names '{ident}', which does not exist in the code")

    # Every public function must be documented.
    for func in sorted(set(re.findall(r"\b((?:cfuture|cpromise)_[a-z_]+)\s*\(", api))):
        if not re.search(r"\b" + re.escape(func) + r"\b", readme):
            problems.append(f"public function '{func}' is not mentioned in README")

    # Mechanisms the code no longer has must not linger in the docs.
    for stale in (r"ref_?count", r"reference[- ]count", r"\bRC ?= ?\d"):
        for match in re.finditer(stale, readme, flags=re.IGNORECASE):
            line_no = readme.count("\n", 0, match.start()) + 1
            problems.append(f"README line {line_no} still says '{match.group(0)}' (slots use hold bits)")

    # Mermaid reads '#' as the start of an entity code and silently drops the rest of the text.
    for block in re.finditer(r"```mermaid\n(.*?)```", readme, flags=re.S):
        if "#" in block.group(1):
            line_no = readme.count("\n", 0, block.start() + block.group(0).index("#")) + 1
            problems.append(f"README line {line_no}: '#' inside a mermaid block truncates the label")

    # Every in-page link must point at a real heading (GitHub slug rules).
    def slug(heading):
        text = re.sub(r"[`*]", "", heading.strip().lower())
        text = re.sub(r"[^a-z0-9_ \-]", "", text)
        return text.replace(" ", "-")

    body = re.sub(r"```.*?```", "", readme, flags=re.S)
    slugs = {slug(h) for h in re.findall(r"^#{1,6} +(.+)$", body, flags=re.M)}
    for anchor in sorted(set(re.findall(r"\]\(#([^)]+)\)", body))):
        if anchor not in slugs:
            problems.append(f"README links to '#{anchor}', which matches no heading")

    # Test suites: each file listed, and the stated suite count correct.
    suites = sorted(glob.glob(os.path.join(SCRIPT_DIR, "tests", "test_*.cpp")))
    for path in suites:
        if os.path.basename(path) not in readme:
            problems.append(f"{os.path.basename(path)} is missing from README")
    for count in re.findall(r"(\d+)[- ](?:dedicated )?suites?\b", readme):
        if int(count) != len(suites):
            problems.append(f"README says {count} suites, there are {len(suites)}")

    # Constants quoted with a value.
    retries = re.search(r"#define CFUTURE_CAS_MAX_RETRIES \(\(uint32_t\)(\d+)U\)", code)
    quoted = re.search(r"`CFUTURE_CAS_MAX_RETRIES` \((\d+) attempts\)", readme)
    if retries and quoted and retries.group(1) != quoted.group(1):
        problems.append(f"README says {quoted.group(1)} CAS retries, code says {retries.group(1)}")

    # Every build.py flag must be documented.
    for flag in sorted(set(re.findall(r'add_argument\(\s*"(--[a-z]+)"', open(__file__).read()))):
        if f"build.py {flag}" not in readme:
            problems.append(f"build.py {flag} is not documented in README")

    # Footprint table must match the built library (same toolchain as the README states).
    lib = os.path.join(SCRIPT_DIR, build_dir, "libcfuture.a")
    if shutil.which("size") and os.path.exists(lib):
        out = subprocess.run(["size", lib], capture_output=True, text=True, check=False).stdout
        compared = 0
        for line in out.splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 6 and parts[5] in ("cfuture.c.o", "cfuture_pal.c.o"):
                text, _data, bss = parts[0], parts[1], parts[2]
                compared += 1
                if not re.search(rf"\b{text}\s+\d+\s+{bss}\s+\d+\s+\w+\s+{re.escape(parts[5])}", readme):
                    problems.append(
                        f"README footprint row for {parts[5]} is stale (now text={text} bss={bss})"
                    )
        if compared != 2:
            # Only GNU binutils `size` lists archive members in this column layout.
            log_warn("'size' output not in GNU per-member format; footprint table NOT checked.")
    else:
        log_warn("libcfuture.a or 'size' not found; skipping footprint comparison.")

    if problems:
        for problem in problems:
            log_error(problem)
        sys.exit(1)
    log_success("README.md is consistent with the code.")


def main():
    parser = argparse.ArgumentParser(
        description="libcfuture Build, Test, Benchmark, and Quality CLI"
    )
    parser.add_argument("--clean", action="store_true", help="Clean all build directories")
    parser.add_argument("--build", action="store_true", help="Configure and build Release binary")
    parser.add_argument("--test", action="store_true", help="Run full unit test suite")
    parser.add_argument("--tsan", action="store_true", help="Run ThreadSanitizer suite")
    parser.add_argument("--asan", action="store_true", help="Run AddressSanitizer and UBSan suite")
    parser.add_argument("--bench", action="store_true", help="Run micro-benchmarks")
    parser.add_argument("--stats", action="store_true", help="Check size footprint and zero heap")
    parser.add_argument("--coverage", action="store_true", help="Generate lcov coverage report")
    parser.add_argument("--lint", action="store_true", help="Run cppcheck and clang-format checks")
    parser.add_argument("--docs", action="store_true", help="Check README.md against the code")
    parser.add_argument(
        "--all",
        action="store_true",
        help="Run full quality pipeline (clean, build, test, tsan, asan, stats, lint, bench)",
    )

    args = parser.parse_args()

    # If no argument specified, show help
    if not any(vars(args).values()):
        parser.print_help()
        sys.exit(0)

    if args.clean:
        clean()

    if args.all:
        clean()
        build_release()
        run_tests()
        run_tsan()
        run_asan()
        run_stats()
        run_lint()
        run_docs_check()
        run_benchmarks()
        log_success("All quality pipeline gates passed successfully!")
        return

    if args.build:
        build_release()

    if args.test:
        run_tests()

    if args.tsan:
        run_tsan()

    if args.asan:
        run_asan()

    if args.bench:
        run_benchmarks()

    if args.stats:
        run_stats()

    if args.coverage:
        run_coverage()

    if args.lint:
        run_lint()

    if args.docs:
        run_docs_check()


if __name__ == "__main__":
    main()
