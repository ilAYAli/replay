#!/usr/bin/env python3
"""Run replay candidate/reference JSONL and summarize failure-suite deltas."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


def run_replay(args: argparse.Namespace, logs: list[str]) -> str:
    cmd = [
        args.replay,
        "--candidate", args.engine_bin,
        "--reference", args.engine_bin,
        "--candidate-uci", f"nnue_file={args.candidate}",
        "--reference-uci", f"nnue_file={args.reference}",
        "--oracle", args.oracle,
        "--oracle-nodes", str(args.oracle_nodes),
        "--threads", str(args.threads),
        "--jobs", str(args.jobs),
        "--jsonl",
    ]
    if args.fixed_nodes > 0:
        cmd.extend(["--fixed-nodes", str(args.fixed_nodes)])
    if args.max_replay_nodes > 0:
        cmd.extend(["--max-replay-nodes", str(args.max_replay_nodes)])
    if args.count > 0:
        cmd.extend(["--count", str(args.count)])
    cmd.extend(logs)
    proc = subprocess.run(
        cmd, check=False, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)
    if args.stderr:
        args.stderr.parent.mkdir(parents=True, exist_ok=True)
        args.stderr.write_text(proc.stderr)
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(proc.returncode)
    return proc.stdout


def summarize(jsonl_text: str) -> tuple[list[dict], dict[str, object]]:
    rows = [
        json.loads(line) for line in jsonl_text.splitlines() if line.strip()
    ]
    diffs = [int(row.get("diff_cp") or 0) for row in rows]
    nonzero = [diff for diff in diffs if diff != 0]
    candidate_better = sum(1 for diff in diffs if diff > 0)
    reference_better = sum(1 for diff in diffs if diff < 0)

    by_log: dict[str, dict[str, object]] = defaultdict(
        lambda: {
            "positions": 0,
            "candidate_better": 0,
            "reference_better": 0,
            "sum_diff_cp": 0,
            "worst_regression_cp": 0,
            "best_gain_cp": 0,
        })
    for row in rows:
        log = row.get("log_path", "")
        diff = int(row.get("diff_cp") or 0)
        item = by_log[log]
        item["positions"] = int(item["positions"]) + 1
        item["sum_diff_cp"] = int(item["sum_diff_cp"]) + diff
        item["worst_regression_cp"] = min(int(item["worst_regression_cp"]), diff)
        item["best_gain_cp"] = max(int(item["best_gain_cp"]), diff)
        if diff > 0:
            item["candidate_better"] = int(item["candidate_better"]) + 1
        elif diff < 0:
            item["reference_better"] = int(item["reference_better"]) + 1

    summary: dict[str, object] = {
        "positions": len(rows),
        "candidate_better": candidate_better,
        "reference_better": reference_better,
        "sum_diff_cp": sum(diffs),
        "median_nonzero_diff_cp": (
            statistics.median(nonzero) if nonzero else 0),
        "worst_regression_cp": min(diffs) if diffs else 0,
        "best_gain_cp": max(diffs) if diffs else 0,
        "logs": by_log,
    }
    return rows, summary


def write_outputs(
        rows: list[dict], summary: dict[str, object],
        output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    jsonl_path = output_dir / "replay_failure_suite.jsonl"
    with jsonl_path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row) + "\n")
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    with (output_dir / "summary.txt").open("w", encoding="utf-8") as handle:
        handle.write(f"positions={summary['positions']}\n")
        handle.write(f"candidate_better={summary['candidate_better']}\n")
        handle.write(f"reference_better={summary['reference_better']}\n")
        handle.write(f"sum_diff_cp={summary['sum_diff_cp']}\n")
        handle.write(
            f"median_nonzero_diff_cp={summary['median_nonzero_diff_cp']}\n")
        handle.write(f"worst_regression_cp={summary['worst_regression_cp']}\n")
        handle.write(f"best_gain_cp={summary['best_gain_cp']}\n")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="*")
    ap.add_argument(
        "--from-jsonl", nargs="+", type=Path,
        help="Summarize pre-generated replay --jsonl output instead of "
             "invoking replay directly, e.g. shard files merged from a "
             "distributed `forge run replay` run")
    ap.add_argument("--candidate", help="Candidate net file")
    ap.add_argument("--reference", help="Reference net file")
    ap.add_argument(
        "--engine-bin", default="~/assets/engines/candidate",
        help="Shared UCI engine executable both nets load into")
    ap.add_argument("--oracle", default="stockfish")
    ap.add_argument("--replay", default="replay")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--fixed-nodes", type=int, default=100000)
    ap.add_argument("--max-replay-nodes", type=int, default=0)
    ap.add_argument("--oracle-nodes", type=int, default=200000)
    ap.add_argument("--count", type=int, default=0)
    default_output_dir = Path(__file__).resolve().parents[1] / "runs"
    ap.add_argument("--output-dir", type=Path, default=default_output_dir)
    ap.add_argument("--stderr", type=Path)
    args = ap.parse_args()

    if args.from_jsonl:
        if args.logs:
            ap.error("logs are ignored with --from-jsonl; drop the positional args")
        jsonl_text = "\n".join(
            path.read_text(encoding="utf-8") for path in args.from_jsonl)
    else:
        if not args.logs:
            ap.error("logs is required unless --from-jsonl is given")
        if not args.candidate or not args.reference:
            ap.error("--candidate and --reference are required unless --from-jsonl is given")
        args.engine_bin = str(Path(args.engine_bin).expanduser())
        jsonl_text = run_replay(args, args.logs)

    rows, summary = summarize(jsonl_text)
    write_outputs(rows, summary, args.output_dir)
    print(f"positions={summary['positions']}")
    print(f"candidate_better={summary['candidate_better']}")
    print(f"reference_better={summary['reference_better']}")
    print(f"sum_diff_cp={summary['sum_diff_cp']}")
    print(f"median_nonzero_diff_cp={summary['median_nonzero_diff_cp']}")
    print(f"worst_regression_cp={summary['worst_regression_cp']}")
    print(f"best_gain_cp={summary['best_gain_cp']}")


if __name__ == "__main__":
    main()
