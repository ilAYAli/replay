# replay

`replay` reads UCI engine logs and reuses the logged positions as a test set.
By default it asks the candidate engine what it would play at each logged
position, then asks an oracle engine to judge that move. With `--reference`, it
compares candidate moves against a baseline engine on the same positions.

This is not a new game played forward. Each replayed position comes from the
original log, even if the candidate move differs from the logged move.

## Replay vs Log Analysis

Default replay asks the current candidate engine what it would play from each
logged position, then judges that candidate move. It answers: would this
engine/config/version choose bad moves on these positions?

`--log` skips the candidate engine and judges the original logged `bestmove`s.
It answers: were the moves actually played in the game bad? Use this mode when
you want a report comparable to Lichess game analysis.

## Defaults

defaults:

```text
--candidate enyo
--candidate-opts ""
--reference-opts ""
--oracle stockfish
--oracle-opts ""
--oracle-nodes 200000
--fixed-nodes 100000
--fixed-movetime 1000
--threads 1
--jobs 1
```

Without `--reference`, replay reports candidate quality against the oracle.
With `--reference`, replay reports whether the candidate or reference made the
better move according to the oracle.

## Arguments

- `--candidate <path>`: engine being tested. Defaults to `enyo`.
- `--candidate-opts <args>`: extra process arguments for the candidate engine.
- `--reference <path>`: baseline engine for candidate-vs-reference comparison.
- `--reference-opts <args>`: extra process arguments for the reference engine.
  If `--reference` is omitted, the reference uses the candidate engine path.
- `--oracle <path>`: judge engine. Defaults to `stockfish`.
- `--oracle-opts <args>`: extra process arguments for the oracle engine.
- `--oracle-nodes N`: oracle node budget. Defaults to `200000`; raise this
  for deeper validation.
- `--oracle-depth N`: oracle depth budget instead of `--oracle-nodes`; `0`
  follows logged depth.
- `--log`: analyze the actual logged moves instead of replaying a candidate.
  This only meaningfully combines with oracle options.
- `--no-analysis`: replay moves without oracle judgment.
- `--moves`: print per-position replay lines.
- `--move N`: start at fullmove `N`.
- `--count N`: analyze at most `N` logged engine moves.
- `--fixed-nodes N`: replay each position with exactly `N` candidate nodes.
  Defaults to `100000`.
- `--fixed-movetime [MS]`: replay each position with fixed movetime. If `MS`
  is omitted, uses `1000`.
- `--max-replay-nodes N`: use logged node counts, capped at `N`; `0` means no
  cap.
- `--time`: replay with the original logged `go wtime/btime/...` command.
- `--threads N`: set engine threads. Defaults to `1` for deterministic gating.
- `--jobs N`: run up to `N` log files in parallel. Defaults to `1`.
- `--csv`: write per-position analysis rows to stdout.
- `--color`: color judgment labels.
- `--verbose`, `-v`: print UCI traffic and FENs.
- `--help`, `-h`: show help.

## Replay One Log

```sh
replay game.log
```

This replays the default candidate engine on every logged position using
`--fixed-nodes 100000`, then asks the oracle to judge the candidate moves.

To pass process arguments to an engine, quote them as one option string:

```sh
replay --candidate ./new-enyo --candidate-opts "--config ~/.config/enyo/new.json" game.log
```

## Compare With A Reference

```sh
replay --candidate ./new-enyo --reference ./old-enyo \
       --candidate-opts "--config ~/.config/enyo/new.json" \
       --reference-opts "--config ~/.config/enyo/old.json" \
       game.log
```

This runs both engines on the same logged positions with the same replay
budget. The oracle evaluates both moves. Positive `diff` means the candidate
was better than the reference; negative `diff` means the candidate was worse.
When comparing two configs of the same engine, `--reference` can be omitted:

```sh
replay --candidate ./enyo \
       --candidate-opts "--config ~/.config/enyo/default.json" \
       --reference-opts "--config ~/.config/enyo/current.json" \
       game.log
```
