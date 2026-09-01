"""One static page beside merged.json.

Presentation only: every number on the page is read from the merged report,
so the page can never disagree with the JSON, and the exit code's reasons are
passed in rather than recomputed. No script, no external assets -- it opens
from a network share or an attachment and shows the same thing.
"""

from __future__ import annotations

import html
from typing import Iterable


def _e(value: object) -> str:
    return html.escape(str(value), quote=True)


def _pct(value: float | None) -> str:
    return "?" if value is None else f"{value:.0%}"


def _row(cells: Iterable[object], header: bool = False) -> str:
    tag = "th" if header else "td"
    return "<tr>" + "".join(f"<{tag}>{_e(c)}</{tag}>" for c in cells) + "</tr>"


def _identity(merged: dict) -> str:
    client = merged.get("convai_client", {})
    dll = client.get("binaries", {}).get("convai_client.dll", {})
    versions = client.get("versions_logged") or ["<none logged>"]
    rows = [
        _row(("plugin git sha", merged.get("git_sha", "?"))),
        _row(("ConvaiClient versions logged", ", ".join(versions))),
        _row(("convai_client.dll", f"{dll.get('sha256', dll.get('status', '?'))[:12]} "
                                    f"{dll.get('mtime', '')}")),
        _row(("launches", f"{merged.get('launches', 0)} "
                          f"({merged.get('launches_without_report', 0)} without report, "
                          f"{merged.get('launches_hung_on_exit', 0)} hung on exit)")),
    ]
    for entry in client.get("stale_against_staged", []):
        rows.append(_row(("STALE binary", f"{entry.get('name')}: loaded "
                                          f"{str(entry.get('loaded'))[:12]}, staged "
                                          f"{str(entry.get('staged'))[:12]}")))
    for change in merged.get("oracle_changed", []):
        rows.append(_row(("ORACLE CHANGED", f"{change.get('name')}: {change.get('was')} -> "
                                            f"{change.get('now')}")))
    return "<table>" + "".join(rows) + "</table>"


def _tiers(merged: dict) -> str:
    rows = [_row(("tier", "status", "result"), header=True)]
    for name, info in merged.get("tiers", {}).items():
        status = info.get("status", "?")
        if status != "ran":
            result = info.get("reason", "")
        elif "total" in info:
            result = f"{info.get('passed', 0)}/{info.get('total', 0)} passed"
            if info.get("failed"):
                result += f", {info['failed']} FAILED"
        elif "tests" in info:
            result = f"{info.get('passed', 0)}/{info.get('tests', 0)} passed"
            if info.get("failed"):
                result += f", {info['failed']} FAILED"
        else:
            result = ""
        rows.append(_row((name, status, result)))
        for failure in info.get("failed_tests", []):
            label = failure.get("name") or failure.get("test") or "?"
            rows.append(_row(("", "FAIL", label)))
    return "<table>" + "".join(rows) + "</table>"


def _scenarios(merged: dict) -> str:
    floors = merged.get("floors", {})
    rows = [_row(("scenario", "passed", "95% CI", "flaky", "could not start", "floor"),
                 header=True)]
    details = []
    for name, data in merged.get("scenarios", {}).items():
        lo, hi = data.get("pass_rate_ci95", [None, None])
        floor = floors.get(name)
        floor_text = ""
        if floor:
            floor_text = (f"{_pct(floor.get('floor'))} "
                          f"{'met' if floor.get('met') else 'BELOW'}")
        rows.append(_row((name, data.get("pass_rate", "?"), f"{_pct(lo)}-{_pct(hi)}",
                          "yes" if data.get("flaky") else "", data.get("setup_failed", 0) or "",
                          floor_text)))
        metrics = data.get("metrics", {})
        notes = data.get("metric_notes", {})
        if metrics:
            metric_rows = [_row(("metric", "n", "min", "median", "max", "covers"), header=True)]
            for metric, stats in sorted(metrics.items()):
                metric_rows.append(_row((metric, stats.get("n", ""), stats.get("min", ""),
                                         stats.get("median", ""), stats.get("max", ""),
                                         notes.get(metric, ""))))
            details.append(f"<details><summary>{_e(name)} metrics</summary><table>"
                           + "".join(metric_rows) + "</table></details>")
    return "<table>" + "".join(rows) + "</table>" + "".join(details)


def _findings(merged: dict) -> str:
    findings = merged.get("findings", [])
    if not findings:
        return "<p>none</p>"
    parts = []
    for finding in findings:
        lo, hi = finding.get("rate_ci95", [None, None])
        parts.append(
            f"<details open><summary><code>{_e(finding.get('dedup_key', '?'))}</code> "
            f"{_e(finding.get('occurrence_rate', '?'))} of runs, 95% CI "
            f"{_e(_pct(lo))}-{_e(_pct(hi))} — {_e(finding.get('summary', ''))}</summary>"
            f"<p>scenarios: {_e(', '.join(finding.get('scenarios', [])))}</p>"
            + "".join(f"<pre>{_e(sample)}</pre>"
                      for sample in finding.get("evidence_samples", []))
            + "</details>")
    return "".join(parts)


def _diff(merged: dict) -> str:
    delta = merged.get("diff")
    if not delta:
        return "<p>no comparable previous sweep</p>"
    lines = [f"<p>vs {_e(delta.get('compared_to_sha', '?'))}: "
             f"{_e(delta.get('scenarios_that_separated', '?'))} of "
             f"{_e(delta.get('scenarios_compared', '?'))} scenarios could separate the two "
             f"sweeps</p>"]
    rows = [_row(("what", "was", "now", "verdict"), header=True)]
    for c in delta.get("scenario_comparisons", []):
        rows.append(_row((c.get("scenario"), c.get("was"), c.get("now"), c.get("verdict"))))
    for c in delta.get("comparisons", []):
        tail = ""
        if c.get("verdict") in ("under-powered", "not-separable"):
            needed = c.get("runs_needed_per_arm")
            tail = f" (would need {needed} runs/arm)" if needed else " (identical rates)"
        rows.append(_row((c.get("dedup_key"), c.get("was"), c.get("now"),
                          f"{c.get('verdict')}{tail}")))
    return "".join(lines) + "<table>" + "".join(rows) + "</table>"


def render(merged: dict, reasons: list[str]) -> str:
    """The page. `reasons` is why the sweep is not a pass; empty means it is."""
    verdict = ("<p class=\"pass\">PASS</p>" if not reasons else
               "<p class=\"fail\">NOT A PASS</p><ul>"
               + "".join(f"<li>{_e(r)}</li>" for r in reasons) + "</ul>")
    return f"""<!doctype html>
<meta charset="utf-8">
<title>ConvaiTests {_e(merged.get('git_sha', ''))}</title>
<style>
body {{ font: 14px/1.4 system-ui, sans-serif; margin: 2em; max-width: 72em; }}
table {{ border-collapse: collapse; margin: 0.5em 0 1em; }}
td, th {{ border: 1px solid #ccc; padding: 0.25em 0.6em; text-align: left; vertical-align: top; }}
th {{ background: #f3f3f3; }}
pre {{ white-space: pre-wrap; background: #f7f7f7; padding: 0.5em; }}
.pass {{ color: #1a7f37; font-size: 1.6em; font-weight: bold; }}
.fail {{ color: #b3261e; font-size: 1.6em; font-weight: bold; }}
details {{ margin: 0.3em 0; }}
</style>
<h1>ConvaiTests sweep</h1>
{verdict}
<h2>Identity</h2>
{_identity(merged)}
<h2>Tiers</h2>
{_tiers(merged)}
<h2>Scenarios</h2>
{_scenarios(merged)}
<h2>Findings</h2>
{_findings(merged)}
<h2>Against the previous sweep</h2>
{_diff(merged)}
"""
