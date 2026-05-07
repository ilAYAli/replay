# replay

Replay Enyo UCI log files and compare the current engine output with the
`bestmove` values recorded in the log. By default, replay also runs a local
Stockfish end analysis of the replayed moves.

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Basic Use

Replay one log:

```sh
replay "Hypersion vs EnyoBot - npmgxvIO.log"
```

Replay without analysis:

```sh
replay --no-analysis "Hypersion vs EnyoBot - npmgxvIO.log"
```

Start at a fullmove number and replay one engine move:

```sh
replay --move 53 --count 1 "Hypersion vs EnyoBot - npmgxvIO.log"
```

`--move N` starts at the first logged position at or after fullmove `N`.

## Analysis Targets

Analyze the move the current replayed engine actually returns. This is the
default and is usually what you want for current bug hunting:

```sh
replay --analysis-target replayed "game.log"
```

Analyze the `bestmove` recorded in the logfile instead:

```sh
replay --analysis-target logged "game.log"
```

## Saving Reports

Replay saves the analysis beside the log by default. If the candidate engine
UCI id contains a git hash, the filename includes both hash and target as
`<name>.<githash>_<target>_analysis`:

```sh
replay "game.log"
```

For example, Enyo `id name Enyo Release v.dcdd1fe (dirty) ...` with the
default replayed target writes `game.dcdd1fe-dirty_replayed_analysis`.
Runs with `--move` or `--count` include those limits in the filename, for
example `game.dcdd1fe-dirty_replayed_move53_count1_analysis`.

Print the analysis without saving it:

```sh
replay --no-save-analysis "game.log"
```

Example report line:

```text
blunder:    c8d7  Kc8-d7    best: c8c7  Kc8-c7    FEN: 2k4R/1p6/4p3/3pP3/8/2P1KB2/3Q4/1q4r1 b - - 16 53
```

## Batch Analysis

If the target is a directory, replay analyzes all `*.log` files in that
directory and writes `analysis.summary` with filename-prefixed issues:

```sh
replay --analysis-target replayed ~/code/cpp/chess/lichess/pgns
```

This also writes one target-specific analysis file per input log for inspection.
If a matching `<name>.<githash>_<target>_analysis` already exists, batch mode
reuses it instead of analysing that log again.

Use logged moves for historical game analysis:

```sh
replay --analysis-target logged ~/code/cpp/chess/lichess/pgns
```

Avoid writing per-game analysis files:

```sh
replay --analysis-target replayed --no-save-analysis ~/code/cpp/chess/lichess/pgns
```

## Reproducing Time-State Issues

Replay with the original `go wtime ...` command from the log:

```sh
replay --time --threads 4 --move 53 --count 1 "game.log"
```

Use this when investigating timeouts, fallback moves, or other state-dependent
behavior where plain FEN is not enough.
