#!/usr/bin/env python3
"""
build.py - Convenience Build, Test, Benchmark, and Quality Automation Script for libcfuture.

Supports Linux, macOS, and Windows environments.
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


def run_soak(seconds=10, build_dir="build"):
    log_info(f"Running hyper-speed soak stress test for {seconds} seconds...")
    if not os.path.exists(os.path.join(SCRIPT_DIR, build_dir)):
        build_release(build_dir)
    soak_bin = os.path.join(SCRIPT_DIR, build_dir, "benchmarks", "bench_stress_soak")
    if IS_WINDOWS:
        soak_bin += ".exe"
    run_cmd([soak_bin, "--duration", str(seconds)])
    log_success(f"Soak test completed cleanly for {seconds}s.")


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
        gcov_tool = "llvm-cov gcov" if shutil.which("llvm-cov") else "gcov"
        cmd = (
            f"lcov --gcov-tool \"{gcov_tool}\" --capture --directory {build_dir} "
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
            "clang-format --dry-run --Werror src/*.c src/adapters/*.c include/*.h include/adapters/*.h tests/*.cpp benchmarks/*.cpp"
        )
        log_success("clang-format check passed.")
    else:
        log_warn("clang-format not installed; skipping format check.")


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
    parser.add_argument(
        "--soak",
        type=int,
        nargs="?",
        const=10,
        default=None,
        help="Run hyper-speed soak test for N seconds (default: 10s)",
    )
    parser.add_argument("--stats", action="store_true", help="Check size footprint and zero heap")
    parser.add_argument("--coverage", action="store_true", help="Generate lcov coverage report")
    parser.add_argument("--lint", action="store_true", help="Run cppcheck and clang-format checks")
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

    if args.soak is not None:
        run_soak(args.soak)

    if args.stats:
        run_stats()

    if args.coverage:
        run_coverage()

    if args.lint:
        run_lint()


if __name__ == "__main__":
    main()
