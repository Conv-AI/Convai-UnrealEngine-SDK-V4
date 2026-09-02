#!/usr/bin/env python3
"""F21: the process exit code has to track the outcome. `python test_exit_code.py`.

Separate from test_run.py because this one launches the engine -- it needs a
built editor, and it is the only check here that does. It needs no backend and
no character: all three launches use scenarios that declare
RequiresLiveConnection() == false, so it is Tier 0.

Deliberately NOT wired into run.py. The report is the result; the exit code is
for humans and CI, and a sweep that started grading the exit code would be one
more thing to keep true (FINDINGS F21).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run  # noqa: E402

# A scenario that passes, one that does not exist, and -- separately -- a
# failing scenario is asked for by name only if the caller supplies one, because
# every red scenario in this suite is something someone is trying to make green.
PASSING = "virtual_mic_adoption"
NO_SUCH_SCENARIO = "no_such_scenario_f21"


def launch(args, scenario: str, out_dir: Path) -> tuple[int, dict | None]:
    report = out_dir / f"{scenario}.json"
    log = out_dir / f"{scenario}.log"
    cmds = f"convai.tests.Run -scenario={scenario} -report={report} quit"
    with log.open("w", encoding="utf-8", errors="replace") as sink:
        proc = subprocess.run(run.scenario_command(args, cmds), stdout=sink,
                              stderr=subprocess.STDOUT, timeout=args.timeout)
    summary = None
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "CONVAI_TESTS summary " in line:
            summary = line.strip()
    return proc.returncode, summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", required=True)
    parser.add_argument("--editor", required=True)
    parser.add_argument("--map", default="/Engine/Maps/Entry")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--arg", action="append", default=[])
    # The suite's reds move; the caller names whichever scenario currently
    # fails without a backend, and the check is skipped when none does.
    parser.add_argument("--failing-scenario", default="")
    args = parser.parse_args()

    failures = []
    # ignore_cleanup_errors: a launch that hung in teardown can outlive the
    # wait and keep its log open, which is F25's business, not this check's.
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        out = Path(tmp)

        code, summary = launch(args, PASSING, out)
        print(f"{PASSING}: exit={code}  {summary}")
        if code != 0:
            failures.append(f"{PASSING} passed but exited {code}")

        code, summary = launch(args, NO_SUCH_SCENARIO, out)
        print(f"{NO_SUCH_SCENARIO}: exit={code}  {summary}")
        # Nothing ran, so nothing was proved; a filter typo must not read as a
        # green run.
        if code == 0:
            failures.append("a launch that selected no scenario exited 0")

        if args.failing_scenario:
            code, summary = launch(args, args.failing_scenario, out)
            print(f"{args.failing_scenario}: exit={code}  {summary}")
            if summary and "failed=0 setup_failed=0" in summary:
                failures.append(f"{args.failing_scenario} passed; pick another")
            elif code == 0:
                failures.append(f"{args.failing_scenario} failed but exited 0")

    for line in failures:
        print(f"FAIL {line}")
    print("exit codes track the outcome" if not failures
          else f"{len(failures)} check(s) failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
