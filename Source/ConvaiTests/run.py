#!/usr/bin/env python3
"""Launch the Unreal test suite N times and merge the runs into one report.

The orchestrator does not assert. Ordering facts on UE delegates are
frame-accurate in-process observations; round-tripping them through a socket
would measure the IPC. Everything here is launch, repeat, merge, diff.

Two rules from the PRD that show up as code below:

  * No silent retry-until-green. A single failure in fifty runs is a finding,
    so every repeat is recorded and the occurrence rate is reported.
  * Findings are deduplicated by root cause. One cause failing forty scenarios
    is one finding with forty occurrences, not forty bugs.

One process per scenario, not per repeat. Two reasons, both paid for in this
project already:

  * Scenarios are not isolated in-process. A scenario that starts recording
    cannot destroy what it spawned -- F20 makes that a crash -- so a leaked
    player component decides whether the next scenario can adopt a microphone.
    virtual_mic_adoption passes alone and fails behind another scenario in the
    same build.
  * A crash used to take the whole run's report with it. Three runs were lost
    that way in one session. Now it costs one scenario.

The price is a process launch per scenario per repeat, which on a map that
connects to the live backend is the dominant cost of a sweep.
"""

from __future__ import annotations

import argparse
import hashlib
import report_html
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

# A run that renders silence passes every audio assertion. UE drops the app
# volume to the unfocused multiplier when the window is not focused, which a
# headless run never is, so it is pinned rather than hoped for. See
# .scratch/test-framework/issues/01-result.md.
UNFOCUSED_VOLUME_OVERRIDE = "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0"

# What UConvaiSubsystem logs at Display when it initialises the client. A sweep
# that cannot name the native library it graded is a sweep whose numbers cannot
# be attributed -- and this project has already published one report against a
# six-hour-old binary without noticing (I1).
VERSION_LINE = "ConvaiClient Version:"

# Loaded by absolute path from <plugin>\Binaries\Win64, whatever is staged in
# Source\ThirdParty. Hashed rather than trusted, for the same reason.
LOADED_ARTIFACTS = ("convai_client.dll", "convai_http_helper.dll", "AECwebrtc.dll")


# A scenario that never started, because an input it needs was not supplied.
# Reported in its own column and kept out of pass_rate's denominator: the suite
# already refuses to call a tier that did not run a pass, and a launch that
# could not run is not a product failure either. It still fails the sweep --
# see the exit code in main().
SETUP_FAILED = "setup-failed"

# How long a report may sit unmodified with the process still alive before the
# launch is called hung and killed.
#
# This is not "how long the write takes" -- it is the whole of teardown after the
# report lands, and teardown is not instant. ConvaiClientImpl::Disconnect waits
# on its own worker for kDisconnectTimeout = 5 s before synthesizing, and the
# game thread sits in that wait inside FEngineLoop::Exit. At 2.0 s this counted
# every launch whose teardown ran long as a hang: measured 0.6-2.2 s over eight
# launches that all exited by themselves, two of them past 2.0 s. See F25.
#
# 15 s clears the DLL's own bound with room for engine shutdown behind it, so
# what is left over is a launch that really is not coming back.
REPORT_SETTLE_S = 15.0


# Two-sided normal quantiles for 95% confidence and 80% power. Spelled out
# rather than imported: the whole statistics section here is three formulas and
# a dependency would be a larger commitment than the code.
Z_ALPHA = 1.959964
Z_POWER = 0.841621


def wilson_interval(hits: int, runs: int) -> tuple[float, float]:
    """95% interval for a rate. Wilson, not hits/runs +/- something.

    The normal approximation is wrong exactly where this project lives: at 0/10
    it produces the interval [0, 0], which reads as "proven absent" for a
    failure that has simply not been seen yet. Wilson gives [0, 0.28] there,
    which is the honest statement.
    """
    if runs <= 0:
        return (0.0, 1.0)
    p = hits / runs
    denom = 1.0 + Z_ALPHA ** 2 / runs
    centre = (p + Z_ALPHA ** 2 / (2 * runs)) / denom
    spread = Z_ALPHA * math.sqrt(p * (1 - p) / runs + Z_ALPHA ** 2 / (4 * runs ** 2)) / denom
    return (max(0.0, centre - spread), min(1.0, centre + spread))


def fisher_exact_two_tailed(a: int, b: int, c: int, d: int) -> float:
    """p for the 2x2 table [[a, b], [c, d]]. Exact, so it is valid at n=10.

    A chi-square here would be invalid at the cell counts this suite produces --
    three leak events across twenty runs (F29) -- and would report a p that
    looks usable.
    """
    n = a + b + c + d
    if n == 0:
        return 1.0
    row1, row2 = a + b, c + d
    col1 = a + c

    def table_p(x: int) -> float:
        return (math.comb(row1, x) * math.comb(row2, col1 - x)) / math.comb(n, col1)

    observed = table_p(a)
    lo = max(0, col1 - row2)
    hi = min(row1, col1)
    # Sum every table at most as likely as the observed one. The epsilon absorbs
    # float error on tables that are equally likely by construction.
    return min(1.0, sum(table_p(x) for x in range(lo, hi + 1)
                        if table_p(x) <= observed * (1 + 1e-9)))


def runs_needed_per_arm(p1: float, p2: float) -> int | None:
    """Runs per arm to separate two rates at 95% confidence and 80% power.

    None when the two rates are equal, because no sample size separates them.
    This is the number that says a comparison was never going to answer the
    question: F29's 10-per-arm sweep needed 35 and the arithmetic was done
    after the runs rather than before.
    """
    if abs(p1 - p2) < 1e-9:
        return None
    pooled = (p1 + p2) / 2.0
    numerator = (Z_ALPHA * math.sqrt(2 * pooled * (1 - pooled)) +
                 Z_POWER * math.sqrt(p1 * (1 - p1) + p2 * (1 - p2))) ** 2
    return max(1, math.ceil(numerator / (p1 - p2) ** 2))


GRADES_IN_ENGINE = ("plugin-side changes: capture routing, the reference feed, APM "
                    "configuration, and whether AEC is enabled at all")
GRADES_OFFLINE = ("canceller changes in convai-livekit-cpp-p: echo subtraction (ERLE), "
                  "near-end preservation, double-talk -- tests/aec_erle_test.cpp")
GRADES_UNIT = ("the Convai module's own logic, in-process and offline: parsers, state "
               "machines, action plans, context formatting -- Source/Convai/Private/Tests")
# Issue 16: the report says which tier it did not cover. F33 is the cost of not
# saying it -- a defect that fails three offline tests held the in-engine
# attenuation at 2.45 dB, so a report from that tier alone reads as healthy
# through a broken canceller.
NOT_COVERED_OFFLINE = ("a canceller regression does not appear anywhere in the in-engine "
                       "numbers, which no longer even report an attenuation: this branch "
                       "builds against a convai_client header that exposes no AEC counters. "
                       "F33 is why that costs nothing -- a defect failing 3 offline tests "
                       "held the in-engine attenuation at 2.45 dB, inside its healthy range")


# Issue 18. The constants that are the oracle. The cheapest path to green is
# weakening an assertion, and a threshold that moved leaves a report identical
# to a real fix -- session 2's fixture check going in as `EchoPeak <= 0.0f` is
# the accidental version of it. The runner fingerprints these and reports a
# change as its own condition; it does not prevent the edit, because a human
# changing a threshold with a reason is the PRD's separate pass.
ORACLE_CONSTANTS = [
    ("Private/Scenarios/ConvaiAecEchoOnlyScenario.cpp", "MinWindowErleDb"),
    ("Private/Scenarios/ConvaiAecEchoOnlyScenario.cpp", "ControlErleToleranceDb"),
    ("Private/Scenarios/ConvaiAecEchoOnlyScenario.cpp", "MinAudibleEchoPeak"),
    ("Private/Scenarios/ConvaiMicNotInReferenceScenario.cpp", "MaxAttenuationDb"),
    ("Private/Scenarios/ConvaiConfiguredLipSyncScenario.cpp", "kMinJawOpen"),
    ("Private/Scenarios/ConvaiConfiguredLipSyncScenario.cpp", "kNeutralJawOpen"),
]


def oracle_fingerprint(tests_root: Path, dll_repo: Path) -> dict:
    """Values where they are legible, hashes where they are not.

    Constants are stored as their source text so a change reports what moved,
    not only that something did. A constant that cannot be found is reported as
    NOT FOUND rather than skipped -- renaming it is the same dodge as changing
    it, and so is deleting the file.
    """
    # Every read is guarded per item: an unreadable file must fingerprint as
    # UNREADABLE and keep the sweep alive, not abort it before any report
    # exists -- the same rule as a launch that produced nothing.
    def read_text(path: Path) -> str | None:
        try:
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return None

    def sha1(path: Path) -> str:
        try:
            return hashlib.sha1(path.read_bytes()).hexdigest()
        except OSError:
            return "UNREADABLE"

    oracle: dict[str, str] = {}
    for rel, name in ORACLE_CONSTANTS:
        text = read_text(tests_root / rel)
        if text is None:
            oracle[name] = "FILE MISSING"
            continue
        match = re.search(rf"constexpr\s+\w+\s+{re.escape(name)}\s*=\s*([^;]+);", text)
        oracle[name] = match.group(1).strip() if match else "NOT FOUND"

    # The arm table decides which arms assert and which expect a leak, so
    # flipping one bool is worth exactly as much to a fix agent as moving a
    # threshold. Whitespace-normalised text, small enough to carry whole.
    arms_text = read_text(tests_root / "Private" / "Scenarios" /
                          "ConvaiAecEchoOnlyScenario.cpp")
    if arms_text is None:
        oracle["kArms"] = "FILE MISSING"
    else:
        match = re.search(r"constexpr FArm kArms\[\] = \{(.*?)\n\s*\};", arms_text, re.DOTALL)
        oracle["kArms"] = " ".join(match.group(1).split()) if match else "NOT FOUND"

    # Recorded fixtures: the PRD names these beside assertions. A re-recorded
    # wav changes what every threshold means.
    data_dir = tests_root / "Data"
    if data_dir.exists():
        for fixture in sorted(data_dir.iterdir()):
            if fixture.is_file():
                oracle[f"fixture:{fixture.name}"] = sha1(fixture)

    # The offline tier's thresholds, including the skipped F12 criterion whose
    # assertion is the acceptance test for that fix. Hashed whole: the file is
    # assertions plus the fixture builder, and both are oracle.
    dll_test = dll_repo / "tests" / "aec_erle_test.cpp"
    oracle["dll:tests/aec_erle_test.cpp"] = sha1(dll_test) if dll_test.exists() \
        else "FILE MISSING"
    return oracle


def graded_binaries(plugin_root: Path) -> dict:
    """What is in the directory the plugin loads from, hashed.

    The version string only exists for a launch that initialised a client, and
    ten of the twenty-two scenarios never do. This is the half that holds for
    every launch: it reads the same absolute path Convai::StartupModule loads
    by, so it answers "which binary" even for a sweep where nothing connected.
    """
    loaded_dir = plugin_root / "Binaries" / "Win64"
    staged_dir = plugin_root / "Source" / "ThirdParty" / "ConvaiWebRTC" / "lib" / "release" / "win64"

    def digest(path: Path) -> dict:
        try:
            data = path.read_bytes()
        except OSError:
            return {"status": "MISSING"}
        return {
            "sha256": hashlib.sha256(data).hexdigest(),
            "bytes": len(data),
            "mtime": time.strftime("%Y-%m-%dT%H:%M:%S",
                                   time.localtime(path.stat().st_mtime)),
        }

    binaries = {name: digest(loaded_dir / name) for name in LOADED_ARTIFACTS}
    # I1: the staged copy is the one an install writes and the loaded copy is
    # the one that decides the result. When they disagree the sweep is grading
    # something other than what was installed, which has happened here before
    # and produced a fully coherent-looking report.
    stale = []
    for name in LOADED_ARTIFACTS:
        staged = digest(staged_dir / name)
        if staged.get("sha256") and staged["sha256"] != binaries[name].get("sha256"):
            stale.append({"name": name,
                          "loaded": binaries[name].get("sha256", binaries[name].get("status")),
                          "staged": staged["sha256"]})
    return {"loaded_from": str(loaded_dir), "staged_in": str(staged_dir),
            "binaries": binaries, "stale_against_staged": stale}


def oracle_changes(oracle: dict, prior: dict | None) -> list[dict]:
    if not prior:
        return []
    return [{"name": key, "was": prior.get(key), "now": oracle.get(key)}
            for key in sorted(set(oracle) | set(prior))
            if prior.get(key) != oracle.get(key)]


def git_sha(repo: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=15,
        )
        return out.stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def git_branch(repo: Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=15,
        )
        return out.stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def run_offline_tier(dll_repo: Path, out_dir: Path) -> dict:
    """Build and run the DLL repo's gtest suite, one process, JSON out.

    Direct exe rather than ctest: the suite is registered per-case via
    gtest_discover_tests, so ctest is 22 launches and --gtest_output through it
    is 22 processes overwriting one file. The data path is a compile-time
    absolute and the DLLs sit next to the exe, so the working directory does
    not matter.

    Every early return is a status that is not "ran". A tier that did not run
    is reported as not run, never as passing -- same rule the merge applies to
    a launch that produced no report.
    """
    tier = {"grades": GRADES_OFFLINE, "dll_repo": str(dll_repo)}
    build_dir = dll_repo / "build" / "windows-x64-tests"
    exe = build_dir / "tests" / "Release" / "aec_erle_test.exe"
    if not build_dir.exists():
        tier.update(status="not-run",
                    reason=f"no build tree at {build_dir}; run scripts\\build.bat tests "
                           f"in the DLL repo once")
        return tier
    tier["dll_git_sha"] = git_sha(dll_repo)

    # Checked before invoking cmake, because the failure otherwise arrives as
    # `MSBUILD : error MSB1009: Project file does not exist` -- which reads as a
    # broken build tree rather than as a checkout that does not carry the oracle.
    # The suite lives on one branch (feat/roster-rooms as of 2026-08-20) and the
    # deployed DLL is built from another, so this is the normal state, not an
    # accident.
    source = dll_repo / "tests" / "aec_erle_test.cpp"
    if not source.exists():
        tier.update(status="not-run",
                    reason=f"no {source.name} in this checkout of the DLL repo (branch "
                           f"{git_branch(dll_repo)}); the offline AEC oracle lives on the "
                           f"branch that carries tests/aec_erle_test.cpp")
        return tier

    # Incremental rebuild of just this target, so the binary tested is the
    # source's current state rather than whenever someone last built. A no-op
    # when nothing changed.
    build_log = out_dir / "dll_build.log"
    try:
        with build_log.open("w", encoding="utf-8", errors="replace") as log:
            rc = subprocess.run(
                ["cmake", "--build", str(build_dir), "--config", "Release",
                 "--target", "aec_erle_test"],
                stdout=log, stderr=subprocess.STDOUT, timeout=600,
            ).returncode
    except (OSError, subprocess.TimeoutExpired) as exc:
        tier.update(status="not-run", reason=f"cmake --build did not complete: {exc}",
                    log=str(build_log))
        return tier
    if rc != 0:
        tier.update(status="build-failed", log=str(build_log),
                    reason="cmake --build failed, so the tier did not run; see the log")
        return tier
    if not exe.exists():
        tier.update(status="not-run", reason=f"build succeeded but no exe at {exe}")
        return tier

    gtest_json = out_dir / "aec_erle_test.json"
    run_log = out_dir / "dll_test.log"
    try:
        with run_log.open("w", encoding="utf-8", errors="replace") as log:
            proc = subprocess.run([str(exe), f"--gtest_output=json:{gtest_json}"],
                                  stdout=log, stderr=subprocess.STDOUT, timeout=300)
    except subprocess.TimeoutExpired:
        tier.update(status="not-run", reason="aec_erle_test.exe timed out", log=str(run_log))
        return tier
    if not gtest_json.exists():
        tier.update(status="not-run", log=str(run_log),
                    reason=f"gtest wrote no JSON (exit {proc.returncode}); see the log")
        return tier

    try:
        data = json.loads(gtest_json.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        tier.update(status="not-run", log=str(run_log),
                    reason=f"gtest JSON unreadable: {exc}")
        return tier
    passed = 0
    failed = []
    skipped = []
    for suite in data.get("testsuites", []):
        for case in suite.get("testsuite", []):
            name = f"{suite.get('name', '?')}.{case.get('name', '?')}"
            if case.get("failures"):
                failed.append({"test": name,
                               "message": case["failures"][0].get("failure", "")[:500]})
            elif case.get("result") == "SKIPPED":
                # Skipped is its own column, never folded into passed: the F12
                # criterion is skipped with its assertion unchanged, and a
                # report that counted it as green would claim the fix landed.
                messages = case.get("skipped") or []
                skipped.append({"test": name,
                                "message": messages[0].get("message", "")[:200]
                                if messages else ""})
            else:
                passed += 1
    tier.update(
        status="ran",
        exit_code=proc.returncode,
        tests=passed + len(failed) + len(skipped),
        passed=passed,
        failed=len(failed),
        skipped=len(skipped),
        failed_tests=failed,
        skipped_tests=skipped,
        gtest_json=str(gtest_json),
        log=str(run_log),
    )
    return tier


def sweep_orphan_editors(project: Path) -> list[dict]:
    """Kill editor processes a dead orchestrator left behind, before launching.

    run_scenario kills its own child once the report lands (F25), but when
    run.py itself dies the child survives, and the next build fails through
    Live Coding with a message that never appears as an error line. Scoped two
    ways rather than by process name alone: the command line must carry this
    project's .uproject, and the parent must be gone. The maintainer's own
    editor (a different project) and a concurrently running sweep (a live
    parent) both survive the filter.
    """
    script = (
        "$all = Get-CimInstance Win32_Process | "
        "Select-Object ProcessId, ParentProcessId, Name, CommandLine; "
        "$alive = @{}; foreach ($p in $all) { $alive[[int64]$p.ProcessId] = $true }; "
        "$out = @(); foreach ($p in $all) { "
        "if ($p.Name -eq 'UnrealEditor-Cmd.exe') { "
        "$out += [pscustomobject]@{ pid = $p.ProcessId; "
        "parent_alive = [bool]$alive[[int64]$p.ParentProcessId]; "
        "cmd = $p.CommandLine } } }; "
        "ConvertTo-Json @($out)"
    )
    try:
        proc = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                              capture_output=True, text=True, timeout=60)
        rows = json.loads(proc.stdout) if proc.stdout.strip() else []
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
        # The sweep is a convenience; a machine where it cannot run should
        # launch anyway and fail the old way rather than not at all.
        return []
    # ConvertTo-Json collapses a lone element to an object and an empty
    # pipeline to nothing; anything but a list is normalised rather than
    # crashing the sweep before a single launch.
    if isinstance(rows, dict):
        rows = [rows]
    elif not isinstance(rows, list):
        rows = []
    needle = str(project).lower().replace("/", "\\")
    killed = []
    for row in rows:
        cmd = (row.get("cmd") or "").lower().replace("/", "\\")
        if needle not in cmd or row.get("parent_alive"):
            continue
        try:
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            f"Stop-Process -Id {int(row['pid'])} -Force"], timeout=60)
            killed.append({"pid": int(row["pid"])})
        except (OSError, subprocess.TimeoutExpired, ValueError):
            continue
    return killed


def build_command(args, exec_cmds: str, *mode: str) -> str:
    """Assemble the command line as one string.

    Passing a list would route through subprocess's list2cmdline, which quotes
    the whole `-ExecCmds=...` token because its value contains spaces. UE then
    receives a mangled value: it runs the console command but exits before the
    next frame, so the scenario never ticks and the run produces no report --
    which reads as a crash rather than as a quoting bug. The form UE accepts is
    `-ExecCmds="<commands>"`, quotes around the value only, so the line is
    assembled here rather than delegated.

    `mode` is what tells a scenario launch from a unit launch: the map, -game
    and the volume pin on one side, -NullRHI and the report path on the other.
    """
    line = " ".join([
        f'"{args.editor}"',
        f'"{args.project}"',
        *mode,
        "-unattended",
        "-nosplash",
        "-stdout",
        "-NoLogTimes",
        # Convai custom params resolve through the command line last, so an
        # A/B on plugin behaviour is one flag rather than one build. The
        # scenario reports which branch it actually took, because a flag the
        # plugin ignored looks identical here.
        *args.arg,
        f'-ExecCmds="{exec_cmds}"',
    ])
    # Never -nosound: the whole Reference Audio path needs a live
    # Audio::FMixerDevice, and without one the suite measures nothing while
    # looking healthy.
    assert "-nosound" not in line
    return line


def scenario_command(args, exec_cmds: str) -> str:
    return build_command(args, exec_cmds, args.map, "-game", f'"{UNFOCUSED_VOLUME_OVERRIDE}"')


# convai.tests.*, not convai.test.*. The shipping Convai module already
# registers Convai.Test.Run from Source/Convai/Private/Tests -- code the design
# believed had never compiled (see FINDINGS F16). It registers first and wins,
# so a colliding name silently runs the wrong harness.
SCENARIO_LINE = "CONVAI_TESTS scenario="


def parse_scenario_line(line: str) -> tuple[str, bool] | None:
    """`CONVAI_TESTS scenario=<name> needs_character=<0|1>` -> (name, needs).

    The flag is read from the scenario itself rather than kept in a list here,
    which would go stale silently. A line without it is read as needing one:
    an older binary should make the runner over-careful, not under-.
    """
    _, sep, rest = line.partition(SCENARIO_LINE)
    if not sep or not rest.strip():
        return None
    name, _, flags = rest.strip().partition(" ")
    if not name:
        return None
    return name, "needs_character=0" not in flags


def discover_scenarios(args, out_dir: Path) -> dict[str, bool]:
    """Ask the built binary what it has, rather than keeping a list in sync.

    Costs one launch. `quit` is a second, comma-separated console command here
    rather than an argument, because List is synchronous and has finished
    logging by the time it runs -- unlike Run, which needs frames to tick.
    """
    log_path = out_dir / "discover.log"
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        subprocess.run(
            scenario_command(args, "convai.tests.List, quit"),
            stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout,
        )
    found: dict[str, bool] = {}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        parsed = parse_scenario_line(line)
        if parsed:
            found[parsed[0]] = parsed[1]
    return found


def configured_character(args) -> str:
    """The character these launches will talk to, from the two places it lives.

    The command line wins in UConvaiUtils::GetTestCharacterID, so it wins here.
    """
    for extra in args.arg:
        prefix, sep, value = extra.partition("=")
        if sep and prefix.lstrip("-").lower() == "convaitestcharacterid" and value.strip():
            return value.strip()
    try:
        ini = (Path(args.project).resolve().parent / "Config" / "DefaultEngine.ini")
        text = ini.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    match = re.search(r"^TestCharacterID=(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else ""


def launch_editor(args, command: str, log_path: Path,
                  report_path: Path) -> tuple[int | None, bool, float]:
    """One editor process, launch to report to exit: (exit code, hung, seconds).

    Raises TimeoutExpired past --timeout, with the process already killed.
    """
    started = time.time()
    hung_after_reporting = False
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        # The report is written before the engine is asked to exit, and a launch
        # that has delivered its result has nothing left to say, so waiting out
        # the full timeout on one that will not come back costs ten minutes.
        #
        # But "will not come back" is a judgement, and this used to make it after
        # 2 s. Teardown is not instant: ConvaiClientImpl::Disconnect waits on its
        # own worker for 5 s, and the game thread sits in that wait inside
        # FEngineLoop::Exit -- which is where a captured stack found it. Eight
        # launches measured without killing anything exited by themselves in
        # 0.6-2.2 s, so at 2 s this reported hangs that were not hangs. See F25
        # and args.report_settle.
        while proc.poll() is None:
            if time.time() - started > args.timeout:
                proc.kill()
                proc.wait()
                raise subprocess.TimeoutExpired(command, args.timeout)
            if report_path.exists() and \
                    time.time() - report_path.stat().st_mtime > args.report_settle:
                if proc.poll() is None:
                    hung_after_reporting = True
                    proc.kill()
                break
            time.sleep(0.5)
        proc.wait()
    return proc.returncode, hung_after_reporting, time.time() - started


def run_scenario(args, index: int, scenario: str, out_dir: Path) -> dict:
    report_path = out_dir / f"report_{index:03d}_{scenario}.json"
    log_path = out_dir / f"run_{index:03d}_{scenario}.log"

    exec_cmds = f"convai.tests.Run -scenario={scenario} -report={report_path} quit"
    exit_code, hung_after_reporting, elapsed = launch_editor(
        args, scenario_command(args, exec_cmds), log_path, report_path)

    # The version the plugin logged for this launch, not for the sweep. Two
    # versions across one sweep is a swapped DLL mid-run, which is worth seeing
    # rather than averaging over.
    versions = []
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        _, sep, rest = line.partition(VERSION_LINE)
        if sep and rest.strip():
            versions.append(rest.strip())

    result = {
        "index": index,
        "scenario": scenario,
        # None for the ten scenarios that never initialise a client; the binary
        # hashes in the merged report cover those.
        "convai_client_version": versions[0] if versions else None,
        # Tracks the outcome since F21 was fixed, but still meaningless when the
        # process was killed after reporting, which is most of them (F25). The
        # report is the result; nothing here grades this.
        "exit_code": exit_code,
        "hung_after_reporting": hung_after_reporting,
        "elapsed_s": round(elapsed, 1),
        "log": str(log_path),
        "report": None,
    }
    if report_path.exists():
        try:
            result["report"] = json.loads(report_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            # A truncated report means the process died mid-write. Surfaced as
            # its own condition rather than folded into "no report", because the
            # two point at different problems. UnicodeDecodeError is caught with
            # it because an unreadable report is the same class of problem, and
            # letting it escape aborted a whole batch on one bad file.
            result["report_error"] = f"malformed report: {exc}"
    return result


# Source/Convai/Private/Tests registers every test under this prefix.
# `Automation RunTests` matches by substring, so the prefix is what keeps a
# --filter inside the plugin: "Room" alone would also run whatever the engine
# has under that name.
UNIT_TEST_PREFIX = "Convai."

# The phrase -TestExit watches for. AutomationCommandline logs it at Display,
# but on UnrealEditor-Cmd it never reaches the -stdout capture, so the tier
# does not look for it there; the flag stays because it costs nothing and an
# engine that does print it exits one tick sooner.
UNIT_EXIT_PHRASE = "Automation Test Queue Empty"

# AutomationControllerManager's per-test line. Gauntlet parses it too, which
# is what makes it a contract rather than a UI string. The state reaches it
# through NameToDisplayString, so NotRun arrives as "Not Run".
UNIT_RESULT_LINE = re.compile(
    r"Test Completed\. Result=\{([^}]+)\} Name=\{(.+?)\} Path=\{(.+?)\}")

# AutomationCommandline's last word, and the only completion marker that does
# reach stdout. Its absence separates "the run ended with nothing to run" from
# "the editor died before the run ended", which point at different fixes.
UNIT_DONE_LINE = re.compile(r"\*\*\*\* TEST COMPLETE\. EXIT CODE: (-?\d+) \*\*\*\*")


def unit_run_completed(text: str) -> bool:
    return UNIT_DONE_LINE.search(text) is not None


def unit_filter(args) -> str:
    """--filter narrows the unit spec only when the unit tier runs alone.

    The flag is shared with the scenario tier, and under --tier all one string
    cannot name both: a scenario name narrows the spec to nothing and a test
    path matches no scenario, so every filter turned one tier red.
    """
    return args.filter if args.tier == "unit" else ""


def unit_test_spec(filter_text: str) -> str:
    text = filter_text.strip()
    if not text:
        return UNIT_TEST_PREFIX
    if text.lower().startswith(UNIT_TEST_PREFIX.lower()):
        return text
    return UNIT_TEST_PREFIX + text


def unit_summary(states: dict[str, str], errors: dict[str, list[str]] | None = None) -> dict:
    """Counts over test path -> EAutomationState name.

    Anything that is neither Success nor Fail -- NotRun, Skipped, InProcess --
    is not_run: it graded nothing and is not folded into passed.
    """
    failed = [{"name": name, "errors": (errors or {}).get(name, [])}
              for name, state in states.items() if state == "Fail"]
    return {
        "total": len(states),
        "passed": sum(1 for s in states.values() if s == "Success"),
        "failed": len(failed),
        "not_run": sum(1 for s in states.values() if s not in ("Success", "Fail")),
        "failed_tests": failed,
    }


def parse_unit_report(data: dict) -> dict:
    """index.json from -ReportExportPath.

    FAutomatedTestPassResults through FJsonObjectConverter: keys lose their
    leading capital, enums are written by name. Counted from `tests[]` rather
    than read off the top-level `succeeded`/`failed`, which split warnings out
    into their own column and do not count Skipped at all.
    """
    states = {}
    errors = {}
    for test in data.get("tests", []):
        name = test.get("fullTestPath") or test.get("testDisplayName", "?")
        states[name] = test.get("state", "NotRun")
        errors[name] = [entry.get("event", {}).get("message", "")
                        for entry in test.get("entries", [])
                        if entry.get("event", {}).get("type") == "Error"]
    return unit_summary(states, errors)


def parse_unit_log(text: str) -> dict:
    states = {path: state.replace(" ", "")
              for state, _, path in UNIT_RESULT_LINE.findall(text)}
    return unit_summary(states)


def run_unit_tier(args, out_dir: Path) -> dict:
    """The Convai module's own automation tests: one editor launch, JSON out.

    Editor context, no map, no RHI: these are in-process tests of parsers and
    state machines, and the scenario tier's -game launch would only add a world
    they never touch. The controller's index.json is the source of truth; the
    per-test log lines stand in when it did not land, so the tier never reports
    "ran" off nothing.

    Every early return is a status that is not "ran", same rule as the DLL tier.
    """
    spec = unit_test_spec(unit_filter(args))
    exec_cmds = f"Automation RunTests {spec}; Quit"
    report_dir = out_dir / "unit"
    report_dir.mkdir(parents=True, exist_ok=True)
    report_path = report_dir / "index.json"
    log_path = out_dir / "unit.log"
    tier = {"grades": GRADES_UNIT, "exec_cmds": exec_cmds, "log": str(log_path)}
    command = build_command(args, exec_cmds, "-nopause", "-NullRHI",
                            f'-ReportExportPath="{report_dir}"',
                            f'-TestExit="{UNIT_EXIT_PHRASE}"')
    try:
        exit_code, hung, elapsed = launch_editor(args, command, log_path, report_path)
    except subprocess.TimeoutExpired:
        tier.update(status="not-run",
                    reason=f"editor did not exit within {args.timeout} s; see the log")
        return tier
    tier.update(exit_code=exit_code, hung_after_reporting=hung, duration_s=round(elapsed, 1))

    summary = None
    if report_path.exists():
        try:
            # The controller writes a UTF-8 BOM; strict utf-8 rejects the file.
            summary = parse_unit_report(
                json.loads(report_path.read_text(encoding="utf-8-sig")))
            tier.update(source="index.json", report=str(report_path))
        except (OSError, json.JSONDecodeError) as exc:
            tier["report_error"] = f"malformed index.json: {exc}"
    if summary is None:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        summary = parse_unit_log(log_text)
        tier.update(source="log", report=str(log_path))
        # Per-test lines that landed before the editor died are a partial run,
        # and a partial run that happened to be all green must not exit 0.
        if not unit_run_completed(log_text):
            tier.update(status="not-run",
                        reason=f"editor exited (code {exit_code}) before the automation "
                               f"run completed, after {summary['total']} per-test "
                               f"line(s); see the log")
            return tier
    if summary["total"] == 0:
        where = ("index.json lists no tests" if tier["source"] == "index.json" else
                 f"no index.json under {report_dir} and no per-test line in the log")
        tier.update(status="not-run",
                    reason=f"{where} (editor exit {exit_code}): the Convai module was built "
                           f"without WITH_TESTS, or `{spec}` matched nothing")
        return tier
    tier.update(status="ran", **summary)
    return tier


def diff_unit_against_previous(unit: dict, history_dir: Path) -> dict | None:
    """Set difference over failed names, no rates: the module's tests are
    deterministic, so one run each side is the whole story.

    Only a prior run of the same spec is comparable -- against a wider one, a
    narrower --filter would read as every other test newly passing.
    """
    if unit.get("status") != "ran":
        return None
    for path in reversed(sorted(history_dir.glob("merged_*.json"))):
        try:
            prior = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        before = prior.get("tiers", {}).get("unit", {})
        if before.get("status") != "ran" or before.get("exec_cmds") != unit.get("exec_cmds"):
            continue
        was = {t["name"] for t in before.get("failed_tests", [])}
        now = {t["name"] for t in unit.get("failed_tests", [])}
        return {"compared_to": path.name,
                "compared_to_sha": prior.get("git_sha", "unknown"),
                "newly_failing": sorted(now - was),
                "newly_passing": sorted(was - now)}
    return None


def merge(runs: list[dict], sha: str, repeats: int, graded: dict | None = None) -> dict:
    total = len(runs)
    # scenario -> status -> count
    outcomes: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    # dedup_key -> aggregated finding
    findings: dict[str, dict] = {}
    # scenario -> metric -> list of values
    metrics: dict[str, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    # scenario -> metric -> what it can and cannot separate. Written by the
    # scenario next to the number (issue 16), identical across runs.
    notes: dict[str, dict[str, str]] = defaultdict(dict)
    crashed = 0

    for run in runs:
        report = run.get("report")
        if not report:
            crashed += 1
            # A launch that produced nothing is that scenario's outcome, not a
            # gap in the denominator. Recorded under the scenario it was asked
            # for, which is knowable now that a process runs exactly one.
            outcomes[run["scenario"]]["no_report"] += 1
            continue
        for scenario in report.get("scenarios", []):
            name = scenario["name"]
            outcomes[name][scenario.get("status", "unknown")] += 1
            for key, value in (scenario.get("metrics") or {}).items():
                if isinstance(value, (int, float)):
                    metrics[name][key].append(float(value))
            notes[name].update(scenario.get("metric_notes") or {})
            for finding in scenario.get("findings", []):
                key = finding.get("dedup_key") or finding.get("summary", "")
                entry = findings.setdefault(key, {
                    "dedup_key": key,
                    "summary": finding.get("summary", ""),
                    "occurrences": 0,
                    "runs_affected": set(),
                    "scenarios": set(),
                    "evidence_samples": [],
                })
                entry["occurrences"] += 1
                entry["runs_affected"].add(run["index"])
                entry["scenarios"].add(name)
                if len(entry["evidence_samples"]) < 3:
                    entry["evidence_samples"].append(finding.get("evidence", ""))

    def summarize(values: list[float]) -> dict:
        ordered = sorted(values)
        return {
            "n": len(ordered),
            "min": ordered[0],
            "max": ordered[-1],
            "median": ordered[len(ordered) // 2],
            "mean": sum(ordered) / len(ordered),
        }

    def attempted(counts: dict[str, int]) -> int:
        """Launches that could have produced an outcome for this scenario."""
        return sum(count for status, count in counts.items() if status != SETUP_FAILED)

    scenario_launches = {name: attempted(counts) for name, counts in outcomes.items()}

    def finding_entry(f: dict) -> dict:
        scenarios = sorted(f["scenarios"])
        hits = len(f["runs_affected"])
        # The launches of the scenarios this finding could have appeared in, not
        # the requested repeat count. A scenario that crashed on half its
        # launches has a smaller denominator than the flag asked for, and
        # reporting 3/10 when only 5 ran overstates how well the rate is known.
        runs = max((scenario_launches.get(s, 0) for s in scenarios), default=0)
        lo, hi = wilson_interval(hits, runs)
        return {**f,
                "scenarios": scenarios,
                "runs_affected": hits,
                "runs": runs,
                "occurrence_rate": f"{hits}/{runs}",
                "rate": hits / runs if runs else 0.0,
                "rate_ci95": [round(lo, 4), round(hi, 4)]}

    return {
        "schema": "convai-tests-merged/6",
        "git_sha": sha,
        # Which native library produced these numbers. A merged report that
        # carries a git SHA, an oracle fingerprint and a tier table but no
        # record of the DLL can be attributed to the plugin and not to it.
        "convai_client": {
            **(graded or {}),
            # Per launch, so a swap mid-sweep shows up as two entries rather
            # than as one arm quietly grading the other's binary.
            "versions_logged": sorted({r["convai_client_version"] for r in runs
                                       if r.get("convai_client_version")}),
            "launches_without_a_version": sum(
                1 for r in runs if not r.get("convai_client_version")),
        },
        "repeats": repeats,
        "launches": total,
        "launches_without_report": crashed,
        # F25's occurrence rate, which is the number that says whether the DLL
        # fix landed.
        "launches_hung_on_exit": sum(1 for r in runs if r.get("hung_after_reporting")),
        "scenarios": {
            name: {
                "outcomes": dict(counts),
                # The number a fix agent acts on. 12/50 and 50/50 are different
                # objects and a fix that moves 12/50 to 0/50 is evidence. The
                # denominator is this scenario's own launches, so a scenario
                # that crashed every time still reports 0/N rather than
                # vanishing from the table.
                "pass_rate": f"{counts.get('pass', 0)}/{attempted(counts)}",
                # What findings have carried since the PRD and scenario
                # outcomes did not: an interval, and a flag once the outcome
                # disagrees with itself. 4/5 reads identically for a scenario
                # that flakes and one that regressed on its last run, and a
                # single-repeat check turns either into a coin flip presented
                # as a verdict (I10).
                "pass_rate_ci95": [round(v, 4) for v in
                                   wilson_interval(counts.get("pass", 0), attempted(counts))],
                "flaky": 0 < counts.get("pass", 0) < attempted(counts),
                # Not a failure and not a pass: the launch never got as far as
                # the product. 3/3 here with 0/0 passed is a sweep that graded
                # nothing, which reads very differently from three reds.
                "setup_failed": counts.get(SETUP_FAILED, 0),
                "metrics": {k: summarize(v) for k, v in metrics[name].items() if v},
                "metric_notes": notes[name],
            }
            for name, counts in outcomes.items()
        },
        # occurrence_rate counts RUNS the finding appeared in, not raw hits: one
        # cause tripping several scenarios in the same run is still one run
        # affected, and "4/2" would otherwise be a legal-looking rate. The raw
        # hit count stays as `occurrences` because breadth across scenarios is
        # its own signal. rate_ci95 is there because 0/10 is not zero.
        "findings": [
            finding_entry(f)
            for f in sorted(findings.values(), key=lambda x: -len(x["runs_affected"]))
        ],
    }


def _passes_and_attempts(report: dict, name: str) -> tuple[int, int]:
    """(passes, launches that could have passed) for one scenario in one report.

    Absent from a report means zero of zero, not zero of the repeat count: a
    scenario that did not exist then cannot be compared, and compare_rates
    reports that as not-comparable rather than as a regression to 0%.
    """
    scenario = report.get("scenarios", {}).get(name)
    if not scenario:
        return (0, 0)
    counts = scenario.get("outcomes", {})
    attempts = sum(count for status, count in counts.items() if status != SETUP_FAILED)
    return (counts.get("pass", 0), attempts)


def parse_floors(specs: list[str]) -> dict[str, float]:
    floors: dict[str, float] = {}
    for spec in specs:
        name, sep, rate = spec.partition("=")
        try:
            value = float(rate)
        except ValueError:
            value = -1.0
        if not sep or not name or not 0.0 <= value <= 1.0:
            raise ValueError(f"--floor wants SCENARIO=RATE with RATE in 0..1, got {spec!r}")
        floors[name] = value
    return floors


def apply_floors(merged: dict, floors: dict[str, float]) -> None:
    """A floor replaces all-must-pass for that scenario only.

    Against a live backend ten repeats of a healthy scenario disagree with
    themselves often enough that "any failure" gates nothing, so a scenario
    with a declared floor is judged on the Wilson lower bound of its pass rate
    instead. Setup failures stay out of the denominator, as everywhere else. A
    floor naming a scenario the sweep did not run is recorded unmet: a typo
    must not read as a pass.
    """
    result = {}
    for name, floor in floors.items():
        data = merged["scenarios"].get(name)
        if data is None:
            result[name] = {"floor": floor, "lower_bound": None, "met": False,
                            "reason": "no such scenario in this sweep"}
            continue
        attempted = int(data["pass_rate"].split("/")[1])
        lower = data["pass_rate_ci95"][0]
        result[name] = {"floor": floor, "lower_bound": lower,
                        "met": attempted > 0 and lower >= floor}
    merged["floors"] = result


def verdict(merged: dict, run_unit: bool, run_engine: bool, run_dll: bool) -> list[str]:
    """Why the sweep is not a pass; empty means it is.

    Non-zero when anything failed or any launch produced no report at all. The
    old in-plugin harness set no exit code, which made it unusable from a
    script (issue 12). A requested tier that did not run counts as failure for
    the same reason a launch without a report does: the alternative is a green
    exit standing on work that never happened. So does a scenario that could
    not start -- it is kept out of pass_rate, not out of the exit code.
    """
    reasons: list[str] = []
    tiers = merged.get("tiers", {})
    if run_unit:
        unit_tier = tiers.get("unit", {})
        if unit_tier.get("status") != "ran" or unit_tier.get("failed", 0) > 0:
            reasons.append(f"tier unit: {unit_tier.get('status')}, "
                           f"{unit_tier.get('failed', 0)} failed")
    if run_dll:
        dll_tier = tiers.get("offline_dll", {})
        if dll_tier.get("status") != "ran" or dll_tier.get("failed", 0) > 0:
            reasons.append(f"tier dll: {dll_tier.get('status')}, "
                           f"{dll_tier.get('failed', 0)} failed")
    if run_engine and tiers.get("in_engine", {}).get("status") != "ran":
        reasons.append(f"tier engine: {tiers.get('in_engine', {}).get('status')}")
    if merged["launches_without_report"] > 0:
        reasons.append(f"{merged['launches_without_report']} launch(es) produced no report")
    floors = merged.get("floors", {})
    for name, counts in merged["scenarios"].items():
        if name in floors:
            continue
        outcomes = counts["outcomes"]
        if outcomes.get("pass", 0) != sum(outcomes.values()):
            line = f"{name}: {counts['pass_rate']} passed"
            if counts["setup_failed"]:
                line += f", {counts['setup_failed']} could not start"
            reasons.append(line)
    for name, floor in floors.items():
        if floor["met"]:
            continue
        if floor.get("lower_bound") is None:
            reasons.append(f"{name}: floor {floor['floor']:.0%} but {floor['reason']}")
        else:
            reasons.append(f"{name}: below floor {floor['floor']:.0%} "
                           f"(95% lower bound {floor['lower_bound']:.0%})")
    return reasons


def compare_rates(was_hits: int, was_runs: int, now_hits: int, now_runs: int,
                  min_runs: int) -> tuple[str, int | None, float | None]:
    """One verdict rule, whether the subject is a finding or a scenario.

    `improved` and `worse` are named from the point of view of the rate going
    down, so callers pass the rate of the thing they do not want.
    """
    if was_runs == 0 or now_runs == 0:
        return "not-comparable", None, None
    p = fisher_exact_two_tailed(was_hits, was_runs - was_hits,
                                now_hits, now_runs - now_hits)
    needed = runs_needed_per_arm(was_hits / was_runs, now_hits / now_runs)
    if p < 0.05:
        return ("improved" if now_hits / now_runs < was_hits / was_runs else "worse"), needed, p
    if min(was_runs, now_runs) < min_runs:
        return "under-powered", needed, p
    return "not-separable", needed, p


def _hits_and_runs(report: dict, key: str, scenarios: list[str]) -> tuple[int, int]:
    """This report's (hits, runs) for a finding, present or not.

    A finding's absence from a report is only informative next to how many times
    the scenario that produces it actually ran, which is why the denominator is
    dug out of the scenario table rather than assumed.
    """
    for f in report.get("findings", []):
        if f["dedup_key"] == key:
            return (f.get("runs_affected", 0), f.get("runs", 0))
    table = report.get("scenarios", {})
    runs = 0
    for name in scenarios:
        entry = table.get(name)
        if entry:
            runs = max(runs, sum(entry.get("outcomes", {}).values()))
    return (0, runs)


def diff_against_previous(merged: dict, history_dir: Path, min_runs: int) -> dict | None:
    """Compare rates, not sets of dedup_key.

    The set difference this replaced called a finding "resolved" the moment it
    stopped appearing, so a lucky 0/10 on an intermittent failure was
    indistinguishable from a fix. F26 reads 2/9, 0/10 and 3/10 across three
    sweeps of unchanged code; every one of those transitions would have been
    reported as a resolution or a regression.

    Every comparison carries a verdict, and the verdict is allowed to be "this
    sample cannot answer it" -- with the number of runs per arm that could.

    Compares engine sweeps to engine sweeps. A --tier dll run writes a merged
    report with no scenario table, and diffing rates against it would turn
    every verdict into not-comparable each time a DLL quick-check lands between
    two sweeps.
    """
    if not merged.get("scenarios"):
        return None
    prior = None
    prior_name = None
    for path in reversed(sorted(history_dir.glob("merged_*.json"))):
        try:
            candidate = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if candidate.get("scenarios"):
            prior = candidate
            prior_name = path.name
            break
    if prior is None:
        return None

    keys = {f["dedup_key"]: f.get("scenarios", []) for f in prior.get("findings", [])}
    for f in merged.get("findings", []):
        keys.setdefault(f["dedup_key"], f.get("scenarios", []))

    comparisons = []
    for key in sorted(keys):
        scenarios = keys[key]
        was_hits, was_runs = _hits_and_runs(prior, key, scenarios)
        now_hits, now_runs = _hits_and_runs(merged, key, scenarios)
        verdict, needed, p = compare_rates(was_hits, was_runs, now_hits, now_runs, min_runs)
        lo, hi = wilson_interval(now_hits, now_runs)
        comparisons.append({
            "dedup_key": key,
            "was": f"{was_hits}/{was_runs}",
            "now": f"{now_hits}/{now_runs}",
            "now_ci95": [round(lo, 4), round(hi, 4)],
            "p_value": None if p is None else round(p, 4),
            "verdict": verdict,
            # None means the two observed rates are identical, so no sample size
            # separates them. A number means this sweep was too small and says
            # by how much.
            "runs_needed_per_arm": needed,
        })

    # I9: the same verdict machinery over the scenario table. Twelve scenarios
    # that pass on both sides and five that fail on both cannot distinguish the
    # two sweeps from each other -- they would return the same result against
    # any build that loads. A report that does not say so lets a green sweep
    # read as evidence of equivalence when it is mostly evidence of
    # insensitivity.
    scenario_comparisons = []
    for name in sorted(set(prior["scenarios"]) | set(merged["scenarios"])):
        was = _passes_and_attempts(prior, name)
        now = _passes_and_attempts(merged, name)
        # Failures, not passes, so `improved` keeps meaning "less of the thing
        # nobody wants" the way it does for findings.
        verdict, needed, p = compare_rates(was[1] - was[0], was[1],
                                           now[1] - now[0], now[1], min_runs)
        lo, hi = wilson_interval(now[0], now[1])
        scenario_comparisons.append({
            "scenario": name,
            "was": f"{was[0]}/{was[1]}",
            "now": f"{now[0]}/{now[1]}",
            "now_pass_ci95": [round(lo, 4), round(hi, 4)],
            "p_value": None if p is None else round(p, 4),
            "verdict": verdict,
            "runs_needed_per_arm": needed,
        })

    separated = sum(1 for c in scenario_comparisons if c["verdict"] in ("improved", "worse"))
    return {
        "compared_to": prior_name,
        "compared_to_sha": prior.get("git_sha", "unknown"),
        "min_runs_for_a_verdict": min_runs,
        "comparisons": comparisons,
        "scenario_comparisons": scenario_comparisons,
        "scenarios_compared": len(scenario_comparisons),
        # The number that says what the sweep was capable of, rather than what
        # it happened to see.
        "scenarios_that_separated": separated,
    }


def splice_arg_values(argv: list[str]) -> list[str]:
    """Rewrite `--arg -Foo` into `--arg=-Foo` before argparse sees it.

    Every editor argument worth passing through starts with a dash, and
    argparse reads a dashed value as the next option rather than as this
    option's value -- so the form `--arg`'s own help text documents fails with
    "expected one argument". The `=` spelling is the only one argparse accepts,
    so the separated form is spliced into it here rather than documented away.
    """
    spliced: list[str] = []
    pending = list(argv)
    while pending:
        token = pending.pop(0)
        if token == "--arg" and pending:
            token = f"--arg={pending.pop(0)}"
        spliced.append(token)
    return spliced


# "both" predates the unit tier and keeps meaning engine + dll for the callers
# that already spell it.
TIER_CHOICES = {
    "unit": ("unit",),
    "engine": ("engine",),
    "dll": ("dll",),
    "both": ("engine", "dll"),
    "all": ("unit", "engine", "dll"),
}


def main() -> int:
    plugin_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", type=Path,
                        default=os.environ.get("CONVAI_UE_EDITOR",
                                               r"C:\Program Files\Epic Games\UE_5.8\Engine"
                                               r"\Binaries\Win64\UnrealEditor-Cmd.exe"))
    parser.add_argument("--project", type=Path, default=os.environ.get("CONVAI_UE_PROJECT"))
    # A bare engine map by default. The dev project's own maps place Convai
    # actors that connect to the live backend on BeginPlay, which turns an
    # offline scenario into a networked one and makes every run cost a
    # handshake. Issue 02's minimal test project replaces this; until then the
    # engine's empty map is the closest thing to one.
    parser.add_argument("--map", default="/Engine/Maps/Entry")
    parser.add_argument("--filter", default="",
                        help="substring of the scenario names to run; under --tier unit, of "
                             "the test paths under Convai. instead")
    parser.add_argument("--arg", action="append", default=[],
                        help="extra argument passed through to the editor, repeatable "
                             "(e.g. --arg -AECReferenceTap=Listener)")
    parser.add_argument("--repeat", type=int, default=10,
                        help="PRD default is 10; findings carry an occurrence rate")
    parser.add_argument("--timeout", type=int, default=600, help="seconds per run")
    # Exposed so the threshold can be measured rather than assumed: it decides
    # launches_hung_on_exit, which is the only number F25 moves.
    parser.add_argument("--report-settle", type=float, default=REPORT_SETTLE_S,
                        metavar="SECONDS",
                        help="how long a report may sit with the process alive before the "
                             "launch is called hung and killed (default %(default)s)")
    parser.add_argument("--min-runs", type=int, default=10,
                        help="below this many runs per arm a rate comparison is reported as "
                             "under-powered rather than given a verdict")
    parser.add_argument("--out", type=Path, default=plugin_root / ".testruns")
    parser.add_argument("--floor", action="append", default=[], metavar="SCENARIO=RATE",
                        help="judge SCENARIO on the 95%% lower bound of its pass rate instead "
                             "of all-must-pass, repeatable (e.g. --floor text_roundtrip=0.8)")
    parser.add_argument("--tier", choices=list(TIER_CHOICES), default="all",
                        help="which tiers to run: unit (the Convai module's own automation "
                             "tests, one editor launch), engine (scenarios), dll (offline "
                             "AEC); both = engine + dll, all = every tier. A requested tier "
                             "that could not run is a failure, never a silent pass")
    parser.add_argument("--dll-repo", type=Path,
                        default=os.environ.get("CONVAI_DLL_REPO",
                                               r"E:\Livekit\convai-livekit-cpp-p"))
    args = parser.parse_args(splice_arg_values(sys.argv[1:]))
    try:
        floors = parse_floors(args.floor)
    except ValueError as error:
        parser.error(str(error))

    selected = TIER_CHOICES[args.tier]
    run_unit = "unit" in selected
    run_engine = "engine" in selected
    run_dll = "dll" in selected

    if run_engine or run_unit:
        if not args.project:
            parser.error("--project (or CONVAI_UE_PROJECT) is required for the unit and "
                         "in-engine tiers")
        if not Path(args.editor).exists():
            parser.error(f"editor not found: {args.editor}")

    # The editor's working directory is the engine's Binaries folder, not this
    # one, so a relative --out sends the report somewhere the runner never looks
    # and the launch reports "no report" -- which reads as a crash.
    args.out = Path(args.out).resolve()

    sha = git_sha(plugin_root)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.out) / f"{stamp}_{sha}"
    out_dir.mkdir(parents=True, exist_ok=True)

    tiers = {
        "unit": {"status": "not-run", "reason": f"skipped by --tier {args.tier}",
                 "grades": GRADES_UNIT},
        "in_engine": {"status": "not-run", "reason": f"skipped by --tier {args.tier}",
                      "grades": GRADES_IN_ENGINE},
        "offline_dll": {"status": "not-run", "reason": f"skipped by --tier {args.tier}",
                        "grades": GRADES_OFFLINE},
    }

    # Offline first: it is seconds against the engine tier's minutes, and a
    # canceller regression should be visible before the first editor launch.
    if run_dll:
        print("offline tier: aec_erle_test...", flush=True)
        tiers["offline_dll"] = run_offline_tier(Path(args.dll_repo), out_dir)

    orphans = []
    if run_engine or run_unit:
        orphans = sweep_orphan_editors(args.project)
        for orphan in orphans:
            print(f"killed orphaned editor pid={orphan['pid']} "
                  f"(this project, parent dead)", flush=True)

    if run_unit:
        print(f"unit tier: Automation RunTests {unit_test_spec(unit_filter(args))}, once "
              f"(--repeat does not apply; the module's tests are deterministic)...", flush=True)
        tiers["unit"] = run_unit_tier(args, out_dir)

    runs = []
    scenarios = []
    if run_engine:
        discovered = discover_scenarios(args, out_dir)
        scenarios = [s for s in discovered if not args.filter or args.filter in s]
        if not scenarios:
            # Reported in the tier table rather than returned: the unit tier
            # may already have paid for an editor launch, and its result
            # belongs in the merged report either way. The exit code still
            # goes red through the requested-tier rule below.
            tiers["in_engine"]["reason"] = (
                f"no scenario matched --filter {args.filter}" if args.filter else
                "no scenarios discovered; is the ConvaiTests module built?")
            print(f"in-engine tier: {tiers['in_engine']['reason']}", flush=True)
    if scenarios:
        # I12: without a character these scenarios each launch an editor, fail
        # at setup and report it separately, which reads as a sweep of failures
        # (I3) at the cost of one launch apiece. The value is an environment
        # credential the framework deliberately does not hold, so the runner
        # names the spelling instead of guessing one.
        needs_character = sorted(s for s in scenarios if discovered[s])
        if needs_character and not configured_character(args):
            parser.error(
                f"{len(needs_character)} of the {len(scenarios)} selected scenarios need a "
                "character and TestCharacterID is empty. Pass\n"
                '    "--arg=-ConvaiTestCharacterID=<character-id>"\n'
                f"or set TestCharacterID under [/Script/Convai.ConvaiSettings] in the project's "
                f"Config/DefaultEngine.ini. Needs one: {', '.join(needs_character)}")
        print(f"{len(scenarios)} scenario(s) x {args.repeat} repeat(s) = "
              f"{len(scenarios) * args.repeat} launches", flush=True)

        for i in range(args.repeat):
            for scenario in scenarios:
                print(f"[{i + 1}/{args.repeat}] {scenario}...", flush=True)
                try:
                    run = run_scenario(args, i, scenario, out_dir)
                except subprocess.TimeoutExpired:
                    # The in-process watchdog aborts at 2x a scenario's deadline;
                    # this is the outer backstop for a process that never got that
                    # far.
                    run = {"index": i, "scenario": scenario, "exit_code": None,
                           "timeout": True, "report": None}
                runs.append(run)
                status = "no report" if not run.get("report") else \
                    f"exit={run['exit_code']}"
                # Surfaced rather than swallowed: the run is usable, but the engine
                # deadlocking on exit is a finding of its own (F25) and a silent
                # workaround would hide how often it fires.
                if run.get("hung_after_reporting"):
                    status += " [hung on exit, killed]"
                print(f"    {status} ({run.get('elapsed_s', '?')}s)", flush=True)
        tiers["in_engine"] = {"status": "ran", "grades": GRADES_IN_ENGINE}

    merged = merge(runs, sha, args.repeat, graded_binaries(plugin_root))
    if tiers["offline_dll"]["status"] != "ran":
        tiers["offline_dll"]["not_covered"] = NOT_COVERED_OFFLINE
    merged["tiers"] = tiers
    if orphans:
        merged["orphaned_editors_killed"] = orphans

    history_dir = Path(args.out) / "history"
    history_dir.mkdir(parents=True, exist_ok=True)
    delta = diff_against_previous(merged, history_dir, args.min_runs)
    if delta:
        merged["diff"] = delta
    unit_delta = diff_unit_against_previous(tiers["unit"], history_dir)
    if unit_delta:
        tiers["unit"]["diff"] = unit_delta

    # Issue 18: "the suite went green" and "the suite went green because a
    # threshold moved" must not be the same observation.
    merged["oracle"] = oracle_fingerprint(plugin_root / "Source" / "ConvaiTests",
                                          Path(args.dll_repo))
    previous = sorted(history_dir.glob("merged_*.json"))
    prior_oracle = None
    if previous:
        try:
            prior_oracle = json.loads(previous[-1].read_text(encoding="utf-8")).get("oracle")
        except (OSError, json.JSONDecodeError):
            prior_oracle = None
    changes = oracle_changes(merged["oracle"], prior_oracle)
    if changes:
        merged["oracle_changed"] = changes

    apply_floors(merged, floors)
    reasons = verdict(merged, run_unit, run_engine, run_dll)

    merged_path = out_dir / "merged.json"
    merged_path.write_text(json.dumps(merged, indent=2), encoding="utf-8")
    shutil.copy(merged_path, history_dir / f"merged_{stamp}_{sha}.json")
    html_path = out_dir / "report.html"
    html_path.write_text(report_html.render(merged, reasons), encoding="utf-8")

    print(f"\nmerged report: {merged_path}")
    print(f"html report:   {html_path}")
    client = merged["convai_client"]
    versions = client["versions_logged"] or ["<none logged>"]
    dll = client["binaries"].get("convai_client.dll", {})
    print(f"  ConvaiClient {', '.join(versions)} "
          f"[{dll.get('sha256', dll.get('status', '?'))[:12]} {dll.get('mtime', '')}]")
    for entry in client.get("stale_against_staged", []):
        print(f"  STALE {entry['name']}: loaded {str(entry['loaded'])[:12]}, "
              f"staged {entry['staged'][:12]} -- the loaded one is what these numbers grade")
    for change in merged.get("oracle_changed", []):
        print(f"  ORACLE CHANGED {change['name']}: {change['was']} -> {change['now']}")
    for tier_name, info in merged["tiers"].items():
        if tier_name == "unit" and info["status"] == "ran":
            line = f"  tier unit: {info['passed']}/{info['total']} passed"
            if info["not_run"]:
                line += f", {info['not_run']} not run"
            if info["failed"]:
                line += f", {info['failed']} FAILED"
            print(f"{line} ({info['duration_s']}s, from {info['source']})")
            for failure in info["failed_tests"]:
                first = f": {failure['errors'][0][:200]}" if failure["errors"] else ""
                print(f"    FAIL {failure['name']}{first}")
            for name in info.get("diff", {}).get("newly_failing", []):
                print(f"    NEWLY FAILING {name} (vs {info['diff']['compared_to_sha']})")
            for name in info.get("diff", {}).get("newly_passing", []):
                print(f"    newly passing {name} (vs {info['diff']['compared_to_sha']})")
        elif info["status"] == "ran" and "tests" in info:
            line = f"  tier {tier_name}: {info['passed']}/{info['tests']} passed"
            if info.get("skipped"):
                line += f", {info['skipped']} skipped"
            if info.get("failed"):
                line += f", {info['failed']} FAILED"
            print(line)
            for failure in info.get("failed_tests", []):
                print(f"    FAIL {failure['test']}")
        elif info["status"] != "ran":
            print(f"  TIER {tier_name} {info['status'].upper()}: {info.get('reason', '')}")
    for name, data in merged["scenarios"].items():
        line = f"  {name}: {data['pass_rate']} passed"
        if data["flaky"]:
            lo, hi = data["pass_rate_ci95"]
            line += f" [FLAKY, 95% CI {lo:.0%}-{hi:.0%}]"
        if data["setup_failed"]:
            line += f", {data['setup_failed']} COULD NOT START"
        floor = merged.get("floors", {}).get(name)
        if floor:
            line += (f" [floor {floor['floor']:.0%}: "
                     f"{'met' if floor['met'] else 'BELOW'}, lower bound "
                     f"{floor['lower_bound']:.0%}]" if floor.get("lower_bound") is not None
                     else f" [floor {floor['floor']:.0%}: {floor['reason']}]")
        print(line)
    for finding in merged["findings"]:
        lo, hi = finding["rate_ci95"]
        print(f"  FINDING {finding['dedup_key']} ({finding['occurrence_rate']}, "
              f"95% CI {lo:.0%}-{hi:.0%}): {finding['summary']}")
    if delta:
        print(f"  vs {delta['compared_to_sha']}:")
        separated = delta["scenarios_that_separated"]
        compared = delta["scenarios_compared"]
        print(f"    {separated} of {compared} scenarios could separate the two sweeps"
              f"{' -- the rest would return this result against any build' if separated < compared else ''}")
        for c in delta["scenario_comparisons"]:
            if c["verdict"] in ("improved", "worse", "not-comparable"):
                print(f"    {c['scenario']}: {c['was']} -> {c['now']} passed [{c['verdict']}]")
        for c in delta["comparisons"]:
            needed = c["runs_needed_per_arm"]
            tail = ""
            if c["verdict"] in ("under-powered", "not-separable"):
                tail = (f", would need {needed} runs/arm" if needed
                        else ", identical rates")
            print(f"    {c['dedup_key']}: {c['was']} -> {c['now']} "
                  f"[{c['verdict']}{tail}]")

    if reasons:
        print("  NOT A PASS:")
        for reason in reasons:
            print(f"    {reason}")
    return 1 if reasons else 0


if __name__ == "__main__":
    sys.exit(main())
