# replay

Replay UCI engine logs and compare the candidate engine's current move choices
with the logged moves.

## Replay Model

`replay` uses the log as a position set for fixed-budget engine comparison:

defaults:

```text
--candidate enyo
--oracle stockfish
--oracle-nodes 200000
--fixed-nodes 100000
--fixed-movetime 1000
--threads 1
--jobs 1
```

- `position ...` is sent exactly as logged.
- Logged `setoption ...` lines are sent before replay starts.
- Default candidate replay is `--fixed-nodes 100000`.
- `--max-replay-nodes N` switches to logged-node replay capped at N. Use `0`
  for the full logged node count.
- `--fixed-nodes N` ignores logged nodes and sends exactly N nodes for every
  position.
- `--fixed-movetime [MS]` ignores logged nodes and sends exactly one fixed
  movetime for every position. Without an argument it uses 1000 ms.
- `--time` always sends the original logged `go wtime/btime/...` command.
- If replay chooses a move different from the log, the engine is restarted
  before the next position so stale state from the diverged line is not reused.
- In candidate/reference comparison mode, both replay engines are restarted
  before each logged position so their hash/history state is symmetric.

Default replay mode does not play a new game forward. It asks what the
candidate engine would play at each logged position independently. Once a
candidate move differs from the logged move, later replay positions still come
from the original log, not from the diverged candidate line.

This is not a reconstruction of the original search budget unless `--time` or
`--max-replay-nodes` is used. The default is for engine gating, not original
game reconstruction.

## Analysis

By default replay asks an oracle engine, `stockfish`, to judge each candidate
move. Replay mode compares the oracle best move from the logged root position
with a `searchmoves <candidate-move>` search from the same root.

`--log` does not run the candidate engine. It analyzes the logged game mainline
with Lichess-style consecutive position evals:

```text
eval(position before move)
eval(position after logged move)
```

Analysis output ends with a single `score: N` line after the `game:` line when
the game result is known. The score is 0-100 and blends mean move accuracy with
harmonic mean move accuracy, so one catastrophic move hurts more than several
small losses. The game result itself is not a direct input to the score.

For old/new engine comparisons, use the log set as positions and run both
engines with the same replay budget, then let the oracle judge both moves:

```sh
replay --candidate ./new-enyo --reference ./old-enyo --csv logs/ > compare.csv
```

In candidate/reference/oracle mode, `diff = reference_loss - candidate_loss`.
Positive diff means the candidate move was better than the reference move.
Negative diff means the candidate was worse. The text summary prints the
non-equal moves, net `diff`, median non-zero diff, worst regression, and best
gain.

`--csv` writes per-position rows to stdout and normal progress/output to stderr
in batch mode.

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
replay --candidate ./build/enyo "game.log"
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

Replay every candidate move with the default fixed node budget:

```sh
replay "game.log"
```

Replay every candidate move with a non-default fixed node budget:

```sh
replay --fixed-nodes 500000 "game.log"
```

Replay every candidate move with a fixed time budget:

```sh
replay --fixed-movetime "game.log"
replay --fixed-movetime 5000 "game.log"
```

Write per-position comparison data:

```sh
replay --candidate ./new-enyo --reference ./old-enyo --csv "game.log" > out.csv
```

Replay without oracle analysis:

```sh
replay --no-analysis "game.log"
```

Use another oracle engine:

```sh
replay --oracle ~/source/stockfish/src/stockfish "game.log"
```

Change oracle budget:

```sh
replay --oracle-nodes 2000000 "game.log"
replay --oracle-depth 16 "game.log"
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
