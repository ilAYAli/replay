# replay

Replay UCI engine logs and compare the candidate engine's current move choices
with the logged moves.

## Replay Model

`replay` tries to mimic the original game search from the log:

- `position ...` is sent exactly as logged.
- Logged `setoption ...` lines are sent before replay starts.
- Default candidate replay uses the last logged `info ... nodes N ...` before
  `bestmove`, and sends `go nodes min(N, 100000000)`.
- If no node count was logged, replay falls back to the original logged `go`
  command.
- `--max-replay-nodes N` changes the candidate replay node cap. Use `0` for
  no cap.
- `--time` always sends the original logged `go wtime/btime/...` command.
- If replay chooses a move different from the log, the engine is restarted
  before the next position so stale state from the diverged line is not reused.

This is not a perfect reconstruction of the original TT/history state, but it
keeps the replay bounded by the logged search effort and avoids cold
`go depth N` searches that can be much slower than the real game.

## Analysis

By default replay also asks a reference engine, `stockfish`, to judge each
candidate move and prints inaccuracies, mistakes, blunders, accuracy, and
average centipawn loss.

Reference analysis defaults to:

```text
go nodes 1000000
```

Any reported inaccuracy, mistake, or blunder is confirmed again at:

```text
go nodes 2000000
```

Explicit `--ref-nodes` or `--ref-depth` values are used exactly and skip the
confirmation pass.

## Cache Files

Replay analysis is saved next to the log as:

```text
game.<analysis-key>_analysis
```

`--log` analysis is saved as:

```text
game.analysis
```

Matching analysis files are reused automatically. Use `--force` to ignore the
cache. Use `--verbose` to print the cache provenance hashes.

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

If `game.pgn` exists next to `game.log`, replay uses its mainline length to
ignore post-game searches left in the log.

## Common Commands

Replay one log:

```sh
replay "game.log"
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

Use the full logged node count without the default 100M cap:

```sh
replay --max-replay-nodes 0 "game.log"
```

Skip per-move output:

```sh
replay --summary-only "game.log"
```

Re-run even if a cache file exists:

```sh
replay --force "game.log"
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

Show cache hashes, UCI traffic, and FENs:

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

The build uses the Enyo source tree at `../enyo` for move notation helpers.
Override it with:

```sh
cmake -S . -B build -DENYO_SOURCE_DIR=/path/to/enyo
```
