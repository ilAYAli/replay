# replay

Replay UCI log searches and compare the current engine output with the
logged moves.

`replay` is for reproducing logged engine behavior. It can also report whether
the candidate engine's replay or log moves are inaccuracies, mistakes, or
blunders according to a reference engine.

## Build

```sh
cmake -S . -B build
cmake --build build
```

By default, the build uses the Enyo source tree at `../enyo` for move
notation helpers. Override with `-DENYO_SOURCE_DIR=/path/to/enyo` if needed.

## Log Requirements

The input log must be a plain UCI transcript with one or more search blocks:

```text
position ...
go ...
info depth ...
bestmove ...
```

`position` may be `position startpos moves ...` or `position fen ...`.
`go` is the original command sent to the engine. By default replay uses the
maximum `info depth` seen before `bestmove` and replays with `go depth N`.
With `--time`, replay sends the original logged `go` command instead.

`bestmove` must be a UCI move. Enyo `EMERGENCY_MOVE: ... move=<uci>` lines are
also treated as logged moves. `setoption ...` lines, if present before the
searches, are sent to the candidate engine before replay starts.
Lines beginning with `WARNING` or `ERROR` are printed even when `--verbose` is
off. Search warnings are printed immediately after the move they belong to.
FEN text inside warnings is hidden unless `--verbose` is used.

FEN in the saved report is optional metadata. Enyo logs include
`search_position start: fen=...`; other UCI logs usually do not. Replay shows
FENs in console output only with `--verbose`.

If the final logged side-to-move clock reaches 1ms and the final move does not
leave checkmate, stalemate, a 50-move draw, or basic insufficient material,
replay adds a final `timeout:` line to the report. This is inferred from the UCI
`go wtime/btime` command; the actual game termination still belongs in PGN or
bot/server logs.
When the final status can be inferred from the log, replay also adds a `game:`
line after any `timeout:` line, from the logged engine's perspective.

If `game.pgn` exists next to `game.log`, replay uses the PGN mainline length
to ignore post-game searches left in the log after the actual final move.

## Examples

Replay one log with `enyo` from `PATH`:

```sh
replay "game.log"
```

Candidate engine initialization is hidden by default; use `--verbose` to print
full UCI traffic, cache hashes, and FENs.
Move output keeps UCI first for grepping, with algebraic notation in
parentheses and the reference engine's best move/score appended when analyzed.

Replay analysis is saved beside the log as `game.<analysis-key>_rpl_analysis`.
Log analysis from `--log` is saved as `game.analysis`.
`replay` exits nonzero when the report contains a blunder or timeout.
The key hashes the log, sibling PGN when present, replay/analysis mode,
reference binary content and UCI output, and content hashes for NNUE/file-valued
reference options that exist locally. For replay analysis it also hashes the
candidate binary content and UCI output plus the exact setoption stream sent by
replay.
If that file already exists, replay reuses it and skips the log.
Limited/debug runs such as `--move` and `--count` are not cached.
Use `--verbose` to print the cache provenance hashes, for example:
`analysis-key 91c8a4d2 | candidate cfg 7d2a4b10 | reference cfg 41f0aa29 | log b13c9a02 | target log | ref-depth 20 | nnue2 d43206fe`.

Replay one log with an explicit engine:

```sh
replay --engine ../assets/engines/enyo_ee7052f "game.log"
```

The engine can also be positional:

```sh
replay ../assets/engines/enyo_ee7052f "game.log"
```

Replay several matching logs; non-`.log` matches are ignored:

```sh
replay *_oot*
```

Pressing `Ctrl+C` during a batch run stops the whole batch.

Start at a fullmove number and replay one logged engine move:

```sh
replay --move 53 --count 1 "game.log"
```

Replay with the original logged time-control command instead of logged depth:

```sh
replay --time --threads 4 --move 53 --count 1 "game.log"
```

Analyze the fixed logged moves without running the candidate engine:

```sh
replay --log "game.log"
```

Suppress individual move lines and print only the final summaries:

```sh
replay --log --summary-only "game.log"
```

With `--log`, `--time` is ignored and replay prints a warning.
`--log` cannot be combined with `--no-analysis`.

Use a specific reference engine and analysis depth. The default reference
analysis depth is 20:

```sh
replay --engine ./build/enyo --reference stockfish --ref-depth 16 "game.log"
```

Follow the logged engine depth for reference analysis:

```sh
replay --ref-depth 0 "game.log"
```

Re-analyze even when a matching analysis file already exists:

```sh
replay --force "game.log"
```

Replay without the end analysis:

```sh
replay --no-analysis "game.log"
```

Color judgement labels in console output:

```sh
replay --color "game.log"
```
