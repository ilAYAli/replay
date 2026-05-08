# replay

Replay Enyo UCI log searches and compare the current engine output with the
`bestmove` values recorded in the log.

`replay` is for reproducing logged engine behavior and reporting whether the
candidate engine's replayed moves are inaccuracies, mistakes, or blunders.

## Build

```sh
cmake -S . -B build
cmake --build build
```

By default, the build uses the Enyo source tree at `../enyo` for move
notation helpers. Override with `-DENYO_SOURCE_DIR=/path/to/enyo` if needed.

## Examples

Replay one log with `enyo` from `PATH`:

```sh
replay "game.log"
```

At startup, replay prints the candidate engine UCI id and initialization
commands so the engine version and applied options are visible.
Move output keeps UCI first for grepping, with algebraic notation in
parentheses and the reference engine's best move/score appended when analyzed.

The end report is saved beside the log as
`game.<engine-tag>_replayed_analysis`. If that file already exists, replay
reuses it and skips the log. Limited/debug runs such as `--move` and `--count`
are not cached. Non-default analysis depths include the depth in the analysis
name, such as `game.<engine-tag>_replayed_depth16_analysis`.

Replay one log with an explicit engine:

```sh
replay --engine ../assets/engines/enyo_ee7052f "game.log"
```

The engine can also be positional:

```sh
replay ../assets/engines/enyo_ee7052f "game.log"
```

Start at a fullmove number and replay one logged engine move:

```sh
replay --move 53 --count 1 "game.log"
```

Print only the logged bestmoves:

```sh
replay --print "game.log"
```

Replay with the original logged time-control command instead of logged depth:

```sh
replay --time --threads 4 --move 53 --count 1 "game.log"
```

Use a specific reference engine and analysis depth. The default reference
analysis depth is 20:

```sh
replay --engine ./build/enyo --reference stockfish --analysis-depth 16 "game.log"
```

Follow the logged engine depth for reference analysis:

```sh
replay --analysis-depth 0 "game.log"
```

Re-analyze even when a matching analysis file already exists:

```sh
replay --force "game.log"
```

Replay without the end analysis:

```sh
replay --no-analysis "game.log"
```

Disable colored console output:

```sh
replay --no-color "game.log"
```
