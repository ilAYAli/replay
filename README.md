# replay

Replay UCI engine logs and compare the candidate engine's current move choices
with the logged moves.

## Replay Model

`replay` uses the log as a position set for fixed-budget engine comparison:

- `position ...` is sent exactly as logged.
- Logged `setoption ...` lines are sent before replay starts.
- Default candidate replay uses the logged node count, capped at 300000.
- `--max-replay-nodes N` changes the logged-node cap. Use `0` for the full
  logged node count.
- `--fixed-nodes N` ignores logged nodes and sends exactly N nodes for every
  position.
- `--time` always sends the original logged `go wtime/btime/...` command.
- If replay chooses a move different from the log, the engine is restarted
  before the next position so stale state from the diverged line is not reused.

Default replay mode does not play a new game forward. It asks what the
candidate engine would play at each logged position independently. Once a
candidate move differs from the logged move, later replay positions still come
from the original log, not from the diverged candidate line.

This is not a reconstruction of the original search budget unless `--time` or
`--max-replay-nodes` is used.

## Analysis

By default replay also asks a reference engine, `stockfish`, to judge each
candidate move. Replay mode compares the reference best move from the logged
root position with a `searchmoves <candidate-move>` search from the same root.
This is meant for comparing engine versions at the same logged positions.

`--log` does not run the candidate engine. It analyzes the logged game mainline
with Lichess-style consecutive position evals:

```text
eval(position before move)
eval(position after logged move)
```

Reference analysis defaults to:

```text
go nodes 200000
```

Analysis output ends with a single `score: N` line after the `game:` line when
the game result is known. The score is 0-100 and blends mean move accuracy with
harmonic mean move accuracy, so one catastrophic move hurts more than several
small losses. The game result itself is not a direct input to the score.

For old/new engine comparisons, use the log set as positions and run both
engines with the same fixed candidate and reference budgets:

```sh
replay --engine ./old-enyo --fixed-nodes 100000 --ref-nodes 1000000 --csv logs/ > old.csv
replay --engine ./new-enyo --fixed-nodes 100000 --ref-nodes 1000000 --csv logs/ > new.csv
```

`--csv` writes per-position rows to stdout.

## Log Requirements

The input must be a UCI transcript containing search blocks:

```text
position ...
go ...
info depth ... nodes ...
bestmove ...
```

Required:

- `position startpos moves ...` or `position fen ...`
- `go ...`
- `bestmove <uci>`

Useful optional lines:

- `info ... nodes N ...` for bounded replay
- `search_position start: fen=...` for FENs in verbose reports
- `WARNING...` / `ERROR...` diagnostics, printed with the related move

## Common Commands

Replay one log:

```sh
replay "game.log"
```

Print per-position replay lines:

```sh
replay --moves "game.log"
```

Replay with an explicit candidate engine:

```sh
replay --engine ./build/enyo "game.log"
```

Analyze logged moves only; do not run the candidate engine:

```sh
replay --log "game.log"
```

Batch replay:

```sh
replay --jobs 4 ~/code/cpp/chess/enyo/bugs
```

Batch replay from a filtered list:

```sh
ls *.log | grep -v _oot | replay --jobs 4
ls *.log | grep -v _oot | replay --jobs 4 -
```

Replay one logged engine move:

```sh
replay --move 53 --count 1 "game.log"
```

Use original logged time-control commands:

```sh
replay --time "game.log"
```

Use logged node counts with a cap:

```sh
replay --max-replay-nodes 5000000 "game.log"
```

Use the full logged node count:

```sh
replay --max-replay-nodes 0 "game.log"
```

Replay every candidate move with a fixed node budget:

```sh
replay --fixed-nodes 100000 "game.log"
```

Write per-position comparison data:

```sh
replay --fixed-nodes 100000 --ref-nodes 1000000 --csv "game.log" > out.csv
```

Replay without reference analysis:

```sh
replay --no-analysis "game.log"
```

Use another reference engine:

```sh
replay --reference ~/source/berserk/src/berserk "game.log"
```

Change reference budget:

```sh
replay --ref-nodes 2000000 "game.log"
replay --ref-depth 16 "game.log"
```

Show UCI traffic and FENs:

```sh
replay --verbose "game.log"
```

Color judgement labels:

```sh
replay --color "game.log"
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

The build uses the Enyo source tree at `../enyo` for move generation helpers.
Override it with:

```sh
cmake -S . -B build -DENYO_SOURCE_DIR=/path/to/enyo
```
