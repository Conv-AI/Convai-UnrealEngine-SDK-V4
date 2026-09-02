#!/usr/bin/env python3
"""Assertions for run.py's own logic. No framework: `python test_run.py`.

The scenarios are the oracle for the product; nothing here asserts on Convai.
What this file covers is the runner itself, where a defect is invisible in the
report it produces -- a mislabelled unit or a flag that silently drops its
value looks exactly like a healthy sweep.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import report_html  # noqa: E402
import run  # noqa: E402


def test_arg_takes_a_value_that_starts_with_a_dash():
    """I11: argparse reads `-Foo` as an option, so `--arg -Foo` never arrived."""
    assert run.splice_arg_values(["--arg", "-AllowStdOutLogVerbosity"]) == \
        ["--arg=-AllowStdOutLogVerbosity"]
    assert run.splice_arg_values(["--arg", "-AECReferenceTap=Listener"]) == \
        ["--arg=-AECReferenceTap=Listener"]


def test_arg_splice_leaves_every_other_token_alone():
    argv = ["--repeat", "3", "--arg=-Already", "--filter", "text_roundtrip",
            "--arg", "-Spliced", "--tier", "engine"]
    assert run.splice_arg_values(argv) == [
        "--repeat", "3", "--arg=-Already", "--filter", "text_roundtrip",
        "--arg=-Spliced", "--tier", "engine",
    ]
    # A trailing bare --arg still has to reach argparse as an error, not be
    # swallowed into a value-less flag.
    assert run.splice_arg_values(["--arg"]) == ["--arg"]


def test_no_metric_is_named_seconds_while_holding_milliseconds():
    """I2: FConvaiTestLatencyTracker records milliseconds, every caller said seconds.

    A grep rather than a report assertion, because the defect is in the label a
    scenario chooses and that is decided at compile time -- by the time a report
    exists the name is already wrong and only the value's magnitude betrays it.
    """
    scenarios = Path(__file__).resolve().parent / "Private" / "Scenarios"
    offenders = []
    for source in sorted(scenarios.rglob("*.cpp")):
        text = source.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            # Every quoted name in a scenario that ends in _seconds is a metric
            # label or a mark; the tracker's unit is ms and the ternary in
            # ConvaiSessionLifecycleScenario puts the label on its own line, so
            # the match is on the string rather than on the call around it.
            if re.search(r'TEXT\("\w+_seconds"\)', line):
                offenders.append(f"{source.name}:{number}")
    assert not offenders, f"latency metrics named *_seconds carry ms: {offenders}"


def test_no_convai_binary_sits_outside_its_load_path():
    """I13: a second convai_client.dll under Context/, in the staging layout.

    The plugin loads by absolute path from Binaries/Win64 and only refreshes it
    when a file is missing (I1), so a copy that a wrong -SourceRoot or a stray
    xcopy can reach is one command away from grading the wrong release. The
    check is on the artifact names rather than on Context/ alone: the defect is
    a shadow copy anywhere the tooling can find it.
    """
    plugin_root = Path(__file__).resolve().parents[2]
    artifacts = {"convai_client.dll", "convai_client_dll.lib", "convai_http_helper.dll",
                 "AECwebrtc.dll", "convai_client.pdb"}
    # Where a copy is meant to be: the staged source, the two load directories,
    # and the gitignored evidence area that keeps the releases under test.
    allowed = [plugin_root / "Source" / "ThirdParty",
               plugin_root / "Binaries",
               plugin_root / ".testruns-ab",
               plugin_root / ".testruns"]
    strays = []
    for path in plugin_root.rglob("*"):
        if path.name not in artifacts or not path.is_file():
            continue
        if any(root in path.parents for root in allowed):
            continue
        strays.append(str(path.relative_to(plugin_root)))
    assert not strays, f"convai binaries outside their load path: {strays}"


def test_a_scenario_that_could_not_start_is_not_counted_as_a_failure():
    """I3: a missing character is a missing input, not a product defect.

    The suite already refuses to report a tier that did not run as passing;
    this is the same rule pointed the other way. The launch still fails the
    sweep -- what changes is which column it lands in and whose denominator it
    is counted against.
    """
    def launch(index, status, findings=()):
        return {"index": index, "scenario": "text_roundtrip",
                "report": {"scenarios": [{"name": "text_roundtrip", "status": status,
                                          "findings": list(findings)}]}}

    merged = run.merge([launch(0, "setup-failed"), launch(1, "setup-failed"),
                        launch(2, "pass")], "deadbeef", 3)
    scenario = merged["scenarios"]["text_roundtrip"]
    assert scenario["setup_failed"] == 2
    assert scenario["pass_rate"] == "1/1", scenario["pass_rate"]
    assert scenario["outcomes"]["setup-failed"] == 2

    # A launch that produced no report at all keeps its place in the
    # denominator: that one is a failure of the run, not of its inputs.
    merged = run.merge([launch(0, "pass"),
                        {"index": 1, "scenario": "text_roundtrip", "report": None}],
                       "deadbeef", 2)
    assert merged["scenarios"]["text_roundtrip"]["pass_rate"] == "1/2"

    # Nothing ran, so nothing passed -- and the rate must not read as 0/0 = fine
    # or as a clean sweep of failures.
    merged = run.merge([launch(0, "setup-failed")], "deadbeef", 1)
    scenario = merged["scenarios"]["text_roundtrip"]
    assert scenario["pass_rate"] == "0/0"
    assert scenario["setup_failed"] == 1

    # A finding's denominator is the launches that could have produced it, so a
    # setup-failed launch must not dilute an occurrence rate either.
    finding = [{"dedup_key": "no-bot-transcript-for-text-prompt", "summary": "x",
                "evidence": "y"}]
    merged = run.merge([launch(0, "setup-failed"), launch(1, "fail", finding)],
                       "deadbeef", 2)
    assert merged["findings"][0]["occurrence_rate"] == "1/1"


def test_the_merged_report_names_the_library_it_graded():
    """I4: a sweep that cannot say which DLL answered cannot attribute anything."""
    def launch(index, version):
        return {"index": index, "scenario": "text_roundtrip", "convai_client_version": version,
                "report": {"scenarios": [{"name": "text_roundtrip", "status": "pass"}]}}

    merged = run.merge([launch(0, "0.2.12.320+3b122f2"), launch(1, "0.2.12.320+3b122f2"),
                        launch(2, None)], "deadbeef", 3, {"loaded_from": "x", "binaries": {}})
    client = merged["convai_client"]
    assert client["versions_logged"] == ["0.2.12.320+3b122f2"]
    # Ten of the twenty-two scenarios never initialise a client, so a launch
    # with no version is normal -- but it is counted, not dropped.
    assert client["launches_without_a_version"] == 1
    assert client["loaded_from"] == "x"

    # Two versions in one sweep is a swap mid-run: both are listed rather than
    # one of them winning silently.
    merged = run.merge([launch(0, "0.1.26.1+fdeec72"), launch(1, "0.2.12.320+3b122f2")],
                       "deadbeef", 2)
    assert merged["convai_client"]["versions_logged"] == ["0.1.26.1+fdeec72",
                                                          "0.2.12.320+3b122f2"]


def test_a_stale_loaded_binary_is_reported_against_the_staged_one():
    """I1: the copy in Binaries wins, whatever an install wrote to ThirdParty."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        loaded = root / "Binaries" / "Win64"
        staged = root / "Source" / "ThirdParty" / "ConvaiWebRTC" / "lib" / "release" / "win64"
        loaded.mkdir(parents=True)
        staged.mkdir(parents=True)
        for name in run.LOADED_ARTIFACTS:
            (loaded / name).write_bytes(b"same")
            (staged / name).write_bytes(b"same")
        assert run.graded_binaries(root)["stale_against_staged"] == []

        (staged / "convai_client.dll").write_bytes(b"a newer release")
        stale = run.graded_binaries(root)["stale_against_staged"]
        assert [e["name"] for e in stale] == ["convai_client.dll"]
        assert stale[0]["loaded"] != stale[0]["staged"]

        # Nothing staged at all is the shipped shape of a binary-only install,
        # not a disagreement -- there is nothing to disagree with.
        (staged / "convai_client.dll").unlink()
        assert run.graded_binaries(root)["stale_against_staged"] == []


def test_the_runner_refuses_a_sweep_it_cannot_run():
    """I12: twelve editors each discovering the same missing credential."""
    line = "LogConvaiTests: Display: CONVAI_TESTS scenario=text_roundtrip needs_character=1"
    assert run.parse_scenario_line(line) == ("text_roundtrip", True)
    assert run.parse_scenario_line(
        "CONVAI_TESTS scenario=connection_invalid_character needs_character=0"
    ) == ("connection_invalid_character", False)
    # An older binary lists a bare name. Read as needing a character, so the
    # runner is over-careful rather than silently launching twelve failures.
    assert run.parse_scenario_line("CONVAI_TESTS scenario=reference_feed") ==         ("reference_feed", True)
    assert run.parse_scenario_line("LogConvaiTests: something else") is None

    class Args:
        arg = ["-AllowStdOutLogVerbosity", "-ConvaiTestCharacterID=903b069a-485d-11f1"]
        project = "Z:/nonexistent/Project.uproject"

    assert run.configured_character(Args()) == "903b069a-485d-11f1"
    Args.arg = ["-ConvaiTestCharacterID="]
    assert run.configured_character(Args()) == ""
    Args.arg = []
    assert run.configured_character(Args()) == ""


def test_a_mixed_scenario_is_marked_flaky():
    """I10: 4/5 reads the same for a flake and for a scenario that regressed once."""
    def launch(index, status):
        return {"index": index, "scenario": "audio_roundtrip",
                "report": {"scenarios": [{"name": "audio_roundtrip", "status": status}]}}

    merged = run.merge([launch(0, "pass"), launch(1, "fail"), launch(2, "pass"),
                        launch(3, "pass"), launch(4, "fail")], "deadbeef", 5)
    scenario = merged["scenarios"]["audio_roundtrip"]
    assert scenario["flaky"] is True
    assert scenario["pass_rate"] == "3/5"
    lo, hi = scenario["pass_rate_ci95"]
    assert lo < 0.6 < hi, (lo, hi)

    # Deterministic in either direction is not flaky, and neither is a single
    # run -- one launch cannot disagree with itself.
    for statuses in (["pass", "pass"], ["fail", "fail"], ["fail"]):
        merged = run.merge([launch(i, s) for i, s in enumerate(statuses)], "deadbeef",
                           len(statuses))
        assert merged["scenarios"]["audio_roundtrip"]["flaky"] is False, statuses

    # A launch that could not start is not evidence of instability.
    merged = run.merge([launch(0, "pass"), launch(1, "setup-failed")], "deadbeef", 2)
    assert merged["scenarios"]["audio_roundtrip"]["flaky"] is False


def test_the_report_says_how_much_of_it_could_separate_anything():
    """I9: a green sweep of insensitive scenarios reads as evidence of equivalence."""
    import tempfile

    def sweep(sha, outcomes):
        runs = []
        for index, (name, status) in enumerate(outcomes):
            runs.append({"index": index, "scenario": name,
                         "report": {"scenarios": [{"name": name, "status": status}]}})
        return run.merge(runs, sha, 1)

    # Same result on both sides for stable_red and stable_green; moved for
    # regressed. Only the third can distinguish the two sweeps.
    was = sweep("aaaaaaa", [("stable_green", "pass"), ("stable_red", "fail"),
                            ("regressed", "pass")] * 6)
    now = sweep("bbbbbbb", [("stable_green", "pass"), ("stable_red", "fail"),
                            ("regressed", "fail")] * 6)

    with tempfile.TemporaryDirectory() as tmp:
        history = Path(tmp)
        (history / "merged_20260101_aaaaaaa.json").write_text(json.dumps(was), encoding="utf-8")
        delta = run.diff_against_previous(now, history, min_runs=5)

    by_name = {c["scenario"]: c for c in delta["scenario_comparisons"]}
    assert by_name["regressed"]["verdict"] == "worse", by_name["regressed"]
    assert by_name["stable_green"]["verdict"] == "not-separable"
    assert by_name["stable_red"]["verdict"] == "not-separable"
    # The headline: how much of the suite could have told the two sweeps apart.
    assert delta["scenarios_that_separated"] == 1
    assert delta["scenarios_compared"] == 3


def test_a_server_error_packet_is_not_logged_and_dropped():
    """F9: both error paths in the subsystem used to dead-end in the log.

    A grep rather than a scenario assertion, because the two halves fail in
    places a scenario cannot reach. `server_error_reaches_game` drives
    IConvaiConnectionInterface::OnServerError directly -- the packet sink is
    private and ADR-0005 will not widen it for a test -- so it proves the
    delegate and the severity split but not the decode above them. This covers
    the decode: that the ErrorResponse case still hands the message on, that
    the hardcoded label is gone, and that OnError is not static again, which is
    what made the fatal path unable to reach a session in the first place.
    """
    convai = Path(__file__).resolve().parent.parent / "Convai"
    subsystem = (convai / "Private" / "ConvaiSubsystem.cpp").read_text(
        encoding="utf-8", errors="replace")
    header = (convai / "Public" / "ConvaiSubsystem.h").read_text(
        encoding="utf-8", errors="replace")

    # Every error-response was filed under this label whatever it said. Matched
    # as the format string rather than as the phrase, so the comment explaining
    # why it is gone does not trip the check that keeps it gone.
    assert not re.search(r'TEXT\("Server compatibility notice', subsystem), \
        "the hardcoded compatibility-notice label is back in ConvaiSubsystem.cpp"

    case = subsystem.split("case EC_PacketType::ErrorResponse:", 1)
    assert len(case) == 2, "no ErrorResponse case in ConvaiSubsystem.cpp"
    body = case[1].split("break;", 1)[0]
    assert "OnServerError(" in body, \
        "the ErrorResponse case no longer routes the message to OnServerError"

    assert not re.search(r"static\s+void\s+OnError\s*\(", header), \
        "OnError is static again, so it cannot reach a session"


def test_the_in_plugin_harness_is_guarded_out_of_shipping():
    """F16: an unguarded file there puts test surface in a customer's game.

    Source/Convai/{Public,Private}/Tests compiles into `Convai`, a Runtime module
    that ships. `#if WITH_TESTS` is what keeps it out of Shipping and Test
    builds -- WITH_TESTS is UBT's own define and one of the few names UHT's
    preprocessor understands, so the UCLASSes in there are scoped rather than
    dropped. A new file without the guard restores a UGameInstanceSubsystem,
    five console commands and a spawnable component to the shipped binary, and
    nothing else would say so.
    """
    convai = Path(__file__).resolve().parent.parent / "Convai"
    unguarded = []
    for folder in ("Public/Tests", "Private/Tests"):
        for source in sorted((convai / folder).rglob("*")):
            if source.suffix not in (".h", ".cpp"):
                continue
            if "#if WITH_TESTS" not in source.read_text(encoding="utf-8", errors="replace"):
                unguarded.append(source.name)
    assert not unguarded, f"in-plugin harness files not guarded by WITH_TESTS: {unguarded}"


def test_the_unit_tier_reads_the_controllers_report():
    """One Success, one Fail with its error, one NotRun -- counted, not trusted.

    The fixture is FAutomatedTestPassResults as FJsonObjectConverter writes it:
    keys with their leading capital dropped, enums by name. The top-level
    counts are deliberately wrong here, because the tier counts `tests[]`
    itself rather than reading them.
    """
    report = {
        "succeeded": 99, "failed": 99, "notRun": 99, "totalDuration": 1.5,
        "tests": [
            {"testDisplayName": "DefaultSendsNothing",
             "fullTestPath": "Convai.Connection.AfkTime.DefaultSendsNothing",
             "state": "Success", "entries": [], "warnings": 0, "errors": 0},
            {"testDisplayName": "ParsesPlutchik",
             "fullTestPath": "Convai.Emotion.ParsesPlutchik",
             "state": "Fail",
             "entries": [
                 {"event": {"type": "Info", "message": "decoding joy"},
                  "filename": "ConvaiEmotionComponentTest.cpp", "lineNumber": 40},
                 {"event": {"type": "Error", "message": "Expected 'joy' but got 'trust'"},
                  "filename": "ConvaiEmotionComponentTest.cpp", "lineNumber": 41},
             ],
             "warnings": 0, "errors": 1},
            {"testDisplayName": "NeverScheduled",
             "fullTestPath": "Convai.Room.NeverScheduled",
             "state": "NotRun", "entries": [], "warnings": 0, "errors": 0},
        ],
    }
    summary = run.parse_unit_report(report)
    assert summary["total"] == 3
    assert summary["passed"] == 1
    assert summary["failed"] == 1
    assert summary["not_run"] == 1
    assert summary["failed_tests"] == [
        {"name": "Convai.Emotion.ParsesPlutchik",
         "errors": ["Expected 'joy' but got 'trust'"]},
    ]


def test_the_unit_tier_falls_back_to_the_per_test_log_lines():
    """No index.json is still a source of truth, from the lines Gauntlet parses."""
    log = "\n".join([
        "LogAutomationController: Display: Test Started. Name={A} Path={Convai.X.A}",
        "LogAutomationController: Display: Test Completed. Result={Success} Name={A} "
        "Path={Convai.X.A}",
        "LogAutomationController: Error: Test Completed. Result={Fail} Name={B} "
        "Path={Convai.X.B}",
        # NameToDisplayString puts a space in NotRun on its way to the log.
        "LogAutomationController: Display: Test Completed. Result={Not Run} Name={C} "
        "Path={Convai.X.C}",
        "LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****",
    ])
    summary = run.parse_unit_log(log)
    assert summary["total"] == 3
    assert summary["passed"] == 1
    assert summary["failed"] == 1
    assert summary["not_run"] == 1
    assert summary["failed_tests"] == [{"name": "Convai.X.B", "errors": []}]
    assert run.parse_unit_log("nothing ran")["total"] == 0


def test_the_unit_spec_stays_inside_the_plugin():
    """RunTests matches by substring, so a bare filter would reach engine tests too."""
    assert run.unit_test_spec("") == "Convai."
    assert run.unit_test_spec("Room") == "Convai.Room"
    assert run.unit_test_spec("convai.room") == "convai.room"
    assert run.unit_test_spec("  Convai.Emotion ") == "Convai.Emotion"


def test_a_filter_narrows_the_unit_spec_only_when_the_unit_tier_runs_alone():
    """Under --tier all, `--filter text_roundtrip` used to run zero unit tests and go red."""
    class Args:
        filter = "Room"
        tier = "unit"

    assert run.unit_filter(Args()) == "Room"
    for tier in ("all", "engine", "both"):
        Args.tier = tier
        assert run.unit_filter(Args()) == "", tier


def _unit_tier_from_log(log_text: str, exit_code: int) -> dict:
    """run_unit_tier over a launch that only wrote this log, no index.json."""
    import tempfile

    class Args:
        editor = "UnrealEditor-Cmd.exe"
        project = "Z:/nonexistent/Project.uproject"
        arg = []
        filter = ""
        tier = "unit"

    def launch(args, command, log_path, report_path):
        log_path.write_text(log_text, encoding="utf-8")
        return exit_code, False, 0.1

    original = run.launch_editor
    run.launch_editor = launch
    try:
        with tempfile.TemporaryDirectory() as tmp:
            return run.run_unit_tier(Args(), Path(tmp))
    finally:
        run.launch_editor = original


def test_an_editor_that_died_mid_run_is_not_blamed_on_with_tests():
    """A startup crash used to read as "built without WITH_TESTS"."""
    tier = _unit_tier_from_log("LogInit: Display: Engine is initializing...\n"
                               "Fatal error!\n", 3)
    assert tier["status"] == "not-run"
    assert tier["source"] == "log"
    assert "before the automation run completed" in tier["reason"], tier["reason"]
    assert "code 3" in tier["reason"]
    assert "WITH_TESTS" not in tier["reason"]

    # Green lines before the death are a partial run, not a pass.
    tier = _unit_tier_from_log(
        "LogAutomationController: Display: Test Completed. Result={Success} Name={A} "
        "Path={Convai.X.A}\n", 3)
    assert tier["status"] == "not-run"
    assert "after 1 per-test line(s)" in tier["reason"], tier["reason"]


def test_a_completed_run_of_nothing_points_at_with_tests():
    tier = _unit_tier_from_log(
        "LogAutomationCommandLine: Display: **** TEST COMPLETE. EXIT CODE: 0 ****\n",
        0)
    assert tier["status"] == "not-run"
    assert "WITH_TESTS" in tier["reason"], tier["reason"]
    assert "`Convai.` matched nothing" in tier["reason"]
    assert run.unit_run_completed("**** TEST COMPLETE. EXIT CODE: -1 ****")
    # The -TestExit phrase is echoed on the command line and never reaches stdout.
    assert not run.unit_run_completed('-TestExit="Automation Test Queue Empty"')


def test_the_controllers_report_is_read_through_its_bom():
    """UE 5.8 writes index.json with a UTF-8 BOM; strict utf-8 rejected it and
    the tier silently fell back to the log."""
    import tempfile

    class Args:
        editor = "UnrealEditor-Cmd.exe"
        project = "Z:/nonexistent/Project.uproject"
        arg = []
        filter = ""
        tier = "unit"

    report = {"tests": [{"fullTestPath": "Convai.X.A", "state": "Success", "entries": []}]}

    def launch(args, command, log_path, report_path):
        log_path.write_text("", encoding="utf-8")
        report_path.write_text(json.dumps(report), encoding="utf-8-sig")
        return 0, False, 0.1

    original = run.launch_editor
    run.launch_editor = launch
    try:
        with tempfile.TemporaryDirectory() as tmp:
            tier = run.run_unit_tier(Args(), Path(tmp))
    finally:
        run.launch_editor = original
    assert tier["status"] == "ran", tier
    assert tier["source"] == "index.json"
    assert tier["passed"] == 1 and "report_error" not in tier



def test_both_keeps_its_meaning_and_all_adds_the_unit_tier():
    """A caller that already spells --tier both must not start launching an editor twice."""
    assert "unit" not in run.TIER_CHOICES["both"]
    assert set(run.TIER_CHOICES["both"]) == {"engine", "dll"}
    assert set(run.TIER_CHOICES["all"]) == {"unit", "engine", "dll"}
    assert run.TIER_CHOICES["unit"] == ("unit",)


def test_the_unit_diff_is_a_set_difference_over_the_same_spec():
    import tempfile

    def unit(spec, failed):
        return {"status": "ran", "exec_cmds": f"Automation RunTests {spec}; Quit",
                "failed_tests": [{"name": n, "errors": []} for n in failed]}

    with tempfile.TemporaryDirectory() as tmp:
        history = Path(tmp)
        (history / "merged_20260101_aaaaaaa.json").write_text(json.dumps(
            {"git_sha": "aaaaaaa", "tiers": {"unit": unit("Convai.", ["Convai.A", "Convai.B"])}}),
            encoding="utf-8")
        delta = run.diff_unit_against_previous(unit("Convai.", ["Convai.B", "Convai.C"]), history)
        assert delta["newly_failing"] == ["Convai.C"]
        assert delta["newly_passing"] == ["Convai.A"]
        assert delta["compared_to_sha"] == "aaaaaaa"
        # A narrower filter is not the same run: nothing to compare against.
        assert run.diff_unit_against_previous(unit("Convai.Room", []), history) is None
        assert run.diff_unit_against_previous({"status": "not-run"}, history) is None


def test_a_floor_replaces_all_must_pass_for_that_scenario_only():
    """Doc section 8: against a live backend "any failure" gates nothing, so a
    declared floor judges a scenario on the Wilson lower bound of its pass
    rate. Scenarios without a floor keep the strict rule, a floor on a name
    the sweep did not run is unmet, and no floors means the old exit code.
    """
    def launch(index, name, status):
        return {"index": index, "scenario": name,
                "report": {"scenarios": [{"name": name, "status": status, "findings": []}]}}

    runs = [launch(i, "a", "pass") for i in range(9)] + [launch(9, "a", "fail"),
                                                          launch(10, "b", "fail")]
    merged = run.merge(runs, "deadbeef", 10)

    reasons = run.verdict(merged, run_unit=False, run_engine=False, run_dll=False)
    assert reasons == ["a: 9/10 passed", "b: 0/1 passed"], reasons

    run.apply_floors(merged, {"a": 0.5})
    assert merged["floors"]["a"]["met"], merged["floors"]
    reasons = run.verdict(merged, run_unit=False, run_engine=False, run_dll=False)
    assert reasons == ["b: 0/1 passed"], reasons

    run.apply_floors(merged, {"a": 0.7, "nope": 0.1})
    reasons = run.verdict(merged, run_unit=False, run_engine=False, run_dll=False)
    assert reasons[0] == "b: 0/1 passed", reasons
    assert reasons[1].startswith("a: below floor 70%"), reasons
    assert "nope" in reasons[2] and "no such scenario" in reasons[2], reasons

    # Setup failures stay out of the floor's denominator, and a scenario that
    # never got as far as the product cannot clear any floor.
    merged = run.merge([launch(0, "a", "setup-failed")], "deadbeef", 1)
    run.apply_floors(merged, {"a": 0.0})
    assert not merged["floors"]["a"]["met"]

    for spec in ("a", "a=", "a=2", "=0.5"):
        try:
            run.parse_floors([spec])
        except ValueError:
            continue
        raise AssertionError(f"{spec!r} was accepted")


def test_the_html_report_only_restates_the_json_and_escapes_it():
    """The page is presentation: no number on it comes from anywhere but the
    merged report, and nothing a scenario or a server said can become markup.
    """
    def launch(index, name, status, findings=()):
        return {"index": index, "scenario": name,
                "report": {"scenarios": [{"name": name, "status": status,
                                          "findings": list(findings),
                                          "metrics": {"reply_ms": 12.5},
                                          "metric_notes": {"reply_ms": "<covers>"}}]}}

    finding = [{"dedup_key": "k", "summary": "<b>bold</b>",
                "evidence": "<script>alert(1)</script>"}]
    merged = run.merge([launch(0, "<s>", "fail", finding), launch(1, "<s>", "pass")],
                       "deadbeef", 2)
    merged["tiers"] = {"unit": {"status": "not-run", "reason": "skipped"}}
    page = report_html.render(merged, ["<why>"])
    assert "<script>" not in page and "&lt;script&gt;" in page
    assert "<b>bold</b>" not in page and "&lt;b&gt;bold&lt;/b&gt;" in page
    assert "&lt;why&gt;" in page and "NOT A PASS" in page
    assert "1/2" in page and "reply_ms" in page and "&lt;covers&gt;" in page
    assert "PASS</p>" in report_html.render(merged, [])


def main() -> int:
    checks = [value for name, value in sorted(globals().items())
              if name.startswith("test_") and callable(value)]
    for check in checks:
        check()
        print(f"ok   {check.__name__}")
    print(f"{len(checks)} check(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
