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

Start after a fullmove number and replay one engine move:

```sh
replay --skip 52 --moves 1 "Hypersion vs EnyoBot - npmgxvIO.log"
```

`--skip N` starts at the first logged position after fullmove `N`, so
`--skip 52` starts at fullmove 53.

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

## Lichess Analysis

Use Lichess's annotated export for the end report:

```sh
replay --lichess-analysis "Hypersion vs EnyoBot - npmgxvIO.log"
```

Lichess analysis always describes the logged game moves, not newly replayed
engine choices.

## Saving Reports

Save the analysis beside the log as `<name>.analysis`:

```sh
replay --save-analysis "game.log"
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

This also writes one `.runlog` per input log for inspection.

Use logged moves for historical game analysis:

```sh
replay --analysis-target logged ~/code/cpp/chess/lichess/pgns
```

Also save each per-game `.analysis` file:

```sh
replay --analysis-target replayed --save-analysis ~/code/cpp/chess/lichess/pgns
```

## Reproducing Time-State Issues

Replay with the original `go wtime ...` command from the log:

```sh
replay --time --threads 4 --skip 52 --moves 1 "game.log"
```

Use this when investigating timeouts, fallback moves, or other state-dependent
behavior where plain FEN is not enough.
