#!/usr/bin/env python3

"""Run the standalone SMT parser probe over one file or a directory tree."""

import argparse
import csv
import json
import os
import signal
import subprocess
import sys
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path
from typing import Dict, Iterator, Optional


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
DEFAULT_PROBE = PROJECT_DIR / "build" / "cpu-debug-local" / "smt_parser_probe"
RESULT_PREFIX = "SMT_PARSER_RESULT\t"
STACK_SIGNALS = {
    getattr(signal, "SIGSEGV", None),
    getattr(signal, "SIGBUS", None),
    getattr(signal, "SIGABRT", None),
}
STACK_SIGNALS.discard(None)

STAT_FIELDS = [
    "var_total",
    "var_bool",
    "var_int",
    "var_real",
    "cmd_assert",
    "cmd_declare_fun",
    "cmd_declare_const",
    "cmd_define_fun",
    "op_le",
    "op_ge",
    "op_eq",
    "op_gt",
    "op_lt",
    "op_and",
    "op_or",
    "op_not",
    "op_imply",
    "op_xor",
    "op_add",
    "op_sub",
    "op_mul",
    "op_div",
    "op_ite",
    "op_let",
    "op_distinct",
    "op_fun_application",
]

CSV_FIELDS = [
    "instance_path",
    "theory",
    "expected_status",
    "parse_success",
    "failure_kind",
    "signal_name",
    "parse_seconds",
    "max_sexpr_depth",
    "diagnostic",
    *STAT_FIELDS,
]


def parse_args() -> argparse.Namespace:
  default_jobs = max(1, min(os.cpu_count() or 1, 8))
  parser = argparse.ArgumentParser(
      description="Run smt_parser_probe over a .smt2 file or every .smt2 file under a directory."
  )
  parser.add_argument("root", type=Path, help="A .smt2 file or a directory containing .smt2 files.")
  parser.add_argument(
      "--jobs",
      type=int,
      default=default_jobs,
      help=f"Maximum parser subprocesses to run at once. Default: {default_jobs}",
  )
  parser.add_argument(
      "--output",
      type=Path,
      default=Path("smt_parser_stats.csv"),
      help="CSV path to write. Default: smt_parser_stats.csv",
  )
  parser.add_argument(
      "--probe",
      type=Path,
      default=DEFAULT_PROBE,
      help=f"Path to smt_parser_probe. Default: {DEFAULT_PROBE}",
  )
  parser.add_argument(
      "--timing-only",
      action="store_true",
      help="Measure parser baseline time without collecting SMT stats.",
  )
  return parser.parse_args()


def validate_args(args: argparse.Namespace) -> Dict[str, Path]:
  root = args.root.expanduser().resolve()
  output = args.output.expanduser().resolve()
  probe = args.probe.expanduser().resolve()

  if not root.exists():
    raise SystemExit(f"Input path does not exist: {root}")
  if root.is_file() and root.suffix != ".smt2":
    raise SystemExit(f"Input file is not a .smt2 file: {root}")
  if args.jobs < 1:
    raise SystemExit("--jobs must be at least 1")
  if not probe.exists():
    raise SystemExit(f"smt_parser_probe does not exist: {probe}")
  if not probe.is_file():
    raise SystemExit(f"smt_parser_probe path is not a file: {probe}")
  if not os.access(probe, os.X_OK):
    raise SystemExit(f"smt_parser_probe is not executable: {probe}")

  return {"root": root, "output": output, "probe": probe}


def iter_smt2_files(root: Path) -> Iterator[Path]:
  if root.is_file():
    yield root
    return
  for path in root.rglob("*.smt2"):
    if path.is_file():
      yield path


def instance_path(root: Path, smt_file: Path) -> str:
  if root.is_file():
    return smt_file.name
  return str(smt_file.relative_to(root))


def max_sexpr_depth(path: Path) -> int:
  depth = 0
  max_depth = 0
  in_comment = False
  in_string = False
  in_bar_symbol = False
  pending_char = ""

  with path.open("r", encoding="utf-8", errors="replace") as input_file:
    while True:
      if pending_char:
        ch = pending_char
        pending_char = ""
      else:
        ch = input_file.read(1)
      if not ch:
        break

      if in_comment:
        if ch == "\n" or ch == "\r":
          in_comment = False
        continue

      if in_string:
        if ch == '"':
          next_ch = input_file.read(1)
          if not next_ch:
            break
          if next_ch == '"':
            continue
          in_string = False
          pending_char = next_ch
        continue

      if in_bar_symbol:
        if ch == "|":
          in_bar_symbol = False
        continue

      if ch == ";":
        in_comment = True
      elif ch == '"':
        in_string = True
      elif ch == "|":
        in_bar_symbol = True
      elif ch == "(":
        depth += 1
        if depth > max_depth:
          max_depth = depth
      elif ch == ")" and depth > 0:
        depth -= 1

  return max_depth


def signal_name(returncode: int) -> str:
  if returncode >= 0:
    return ""
  signum = -returncode
  try:
    return signal.Signals(signum).name
  except ValueError:
    return f"SIG{signum}"


def output_snippet(stdout: str, stderr: str, max_chars: int = 500) -> str:
  lines = [line.strip() for line in stderr.splitlines() + stdout.splitlines() if line.strip()]
  text = " | ".join(lines)
  if len(text) > max_chars:
    return text[: max_chars - 3] + "..."
  return text


def parse_probe_result(stdout: str) -> Optional[Dict[str, object]]:
  for line in reversed(stdout.splitlines()):
    if line.startswith(RESULT_PREFIX):
      return json.loads(line[len(RESULT_PREFIX):])
  return None


def blank_stats(row: Dict[str, object]) -> None:
  for field in STAT_FIELDS:
    row[field] = ""


def run_one(probe: Path, root: Path, smt_file: Path, timing_only: bool) -> Dict[str, object]:
  row: Dict[str, object] = {
      "instance_path": instance_path(root, smt_file),
      "theory": "",
      "expected_status": "",
      "parse_success": 0,
      "failure_kind": "",
      "signal_name": "",
      "parse_seconds": "",
      "max_sexpr_depth": max_sexpr_depth(smt_file),
      "diagnostic": "",
  }
  blank_stats(row)

  start = time.monotonic()
  probe_command = [str(probe)]
  if timing_only:
    probe_command.append("--timing-only")
  probe_command.append(str(smt_file))

  result = subprocess.run(
      probe_command,
      capture_output=True,
      text=True,
      check=False,
  )
  elapsed = time.monotonic() - start
  probe_json = parse_probe_result(result.stdout)

  if probe_json is not None:
    success = bool(probe_json.get("parse_success", False))
    stats_collected = bool(probe_json.get("stats_collected", True))
    row["parse_success"] = 1 if success else 0
    row["failure_kind"] = "" if success else "parser_fail"
    row["parse_seconds"] = probe_json.get("parse_seconds", elapsed)
    row["diagnostic"] = "" if success else str(probe_json.get("diagnostic", ""))
    if stats_collected:
      row["theory"] = str(probe_json.get("theory", ""))
      row["expected_status"] = str(probe_json.get("expected_status", ""))
    if success and stats_collected:
      for field in STAT_FIELDS:
        row[field] = probe_json.get(field, 0)
    return row

  row["parse_seconds"] = elapsed
  row["diagnostic"] = output_snippet(result.stdout, result.stderr)
  if result.returncode < 0:
    signum = -result.returncode
    row["signal_name"] = signal_name(result.returncode)
    row["failure_kind"] = "stack_overflow_suspected" if signum in STACK_SIGNALS else "crash_signal"
  else:
    row["failure_kind"] = "command_error"
  return row


def write_rows_streaming(root: Path, probe: Path, output: Path, jobs: int, timing_only: bool) -> int:
  output.parent.mkdir(parents=True, exist_ok=True)
  processed = 0
  files = iter_smt2_files(root)

  with output.open("w", newline="", encoding="utf-8") as csv_file:
    writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
    writer.writeheader()
    csv_file.flush()

    with ThreadPoolExecutor(max_workers=jobs) as executor:
      pending = set()

      def submit_next() -> bool:
        try:
          smt_file = next(files)
        except StopIteration:
          return False
        pending.add(executor.submit(run_one, probe, root, smt_file, timing_only))
        return True

      for _ in range(jobs):
        if not submit_next():
          break

      while pending:
        done, pending = wait(pending, return_when=FIRST_COMPLETED)
        for future in done:
          row = future.result()
          writer.writerow(row)
          csv_file.flush()
          processed += 1
          submit_next()

  return processed


def main() -> int:
  args = parse_args()
  paths = validate_args(args)
  processed = write_rows_streaming(
      paths["root"],
      paths["probe"],
      paths["output"],
      args.jobs,
      args.timing_only,
  )
  if processed == 0:
    print(f"No .smt2 files found under {paths['root']}", file=sys.stderr)
    return 2
  print(f"Wrote {processed} rows to {paths['output']}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
