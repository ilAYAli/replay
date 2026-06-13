# replay

`replay` reads UCI `.log` files and reuses the logged parent positions as a
test set. It does not play a new game forward.

Default mode asks a candidate engine what it would play from each logged
position, then asks an oracle engine to score the candidate, logged, and best
moves. `--log` skips the candidate and analyzes the logged moves. `--reference`
is only for A/B comparison against a different engine or config.

## Common Commands

Analyze logged moves:

```sh
replay --log game.log
```

Replay the current build:

```sh
replay --candidate ./build/enyo game.log
```

Replay the current build with an alternate UCI option:

```sh
replay --candidate ./build/enyo \
  --candidate-uci "nnue_file=foo.nn" \
  game.log
```

Compare against a stable reference when you need A/B output:

```sh
replay --candidate ./build/enyo \
  --reference ~/assets/engines/reference \
  game.log
```

Compare UCI options only when the engine configuration is part of the test:

```sh
replay \
    --reference ~/assets/engines/reference \
    --candidate ~/assets/engines/reference \
    --reference-uci "nnue_file=$HOME/code/cpp/chess/enyo/net/default.nn" \
    --candidate-uci "nnue_file=$HOME/code/cpp/chess/nnue/runs/d16-rc2-secondhalf-fresh-huber-sign-lr2e7-e10-20260531/train/model.nn" \
    game.log
```

Analyze logged moves with a stronger net as the oracle:

```sh
replay --log \
    --oracle ~/assets/engines/reference \
    --oracle-uci "nnue_file=$HOME/code/cpp/chess/enyo/net/strong.nn" \
    game.log
```

Pass process args only when the engine executable needs them:

```sh
replay --candidate ./build/enyo \
  --reference ~/assets/engines/reference \
  --candidate-opts "--config ~/.config/enyo/net-a.json" \
  --reference-opts "--config ~/.config/enyo/net-b.json" \
  game.log
```

Generate NNUE target JSONL:

```sh
find ~/code/cpp/chess/lichess/logs/loss -name '*.log' | sort |
replay --jsonl \
  --candidate ../assets/engines/reference \
  --top-root-moves 4 \
  --max-moves-per-position 16 \
  --jobs 8 \
  --move 8 \
  - \
  > targets/loss_replay.jsonl \
  2> targets/loss_replay.stderr
```

## JSONL

`--jsonl` writes one `enyo.replay.v1` JSON object per replayed position to
stdout. It is for scored move-set extraction, not quick engine comparison.
Progress and engine summaries go to stderr.

Important invariants:

- `fen` is the parent position before the move.
- `score_pov` is always `"parent"`.
- `score_unit` is `"cp"`.
- Mate scores use `32000 - mate_distance`, so `#4` is `31996` and `#-4` is
  `-31996`.
- Every emitted move is legal in `fen` and has a `score_cp`.
- `best_move`, `candidate_move`, `logged_move`, `oracle_move`, and
  `reference_move` are explicit.
- History-sensitive replay-divergence rows are skipped by default. Use
  `--include-history-sensitive` when testing search-history behavior.
- `moves[]` includes the best/oracle move, logged move, candidate move, root PV
  alternatives, legal checks, legal captures, and legal promotions.
- Duplicate moves are merged by UCI move and keep all applicable origins.
- `moves[].score_source` identifies the scorer, currently `oracle`.
- `moves[].score_limit` records the resolved oracle budget for that score.
- `moves[].score_raw`, `score_kind`, `mate_ply`, and `mate_moves` preserve
  raw score metadata alongside normalized `score_cp`.
- Rows with fewer than two scored legal moves are still written, but tagged
  `insufficient_moves`.

Minimal shape:

```json
{
  "schema": "enyo.replay.v1",
  "score_pov": "parent",
  "max_gap_cp": 800,
  "legal_move_count": 21,
  "fen": "...",
  "candidate_move": "b2a3",
  "oracle_move": "b2c1",
  "best_move": "b2c1",
  "candidate_loss_cp": 678,
  "moves": [
    {
      "move": "b2c1",
      "role": ["oracle", "reference"],
      "origins": ["oracle", "reference"],
      "score_cp": -322,
      "score_raw": "cp -322",
      "score_kind": "cp",
      "rank": 1,
      "legal": true,
      "score_source": "oracle",
      "score_limit": {"kind": "nodes", "value": 200000}
    }
  ]
}
```

Move selection defaults are meant for training target extraction:

- `--top-root-moves 12`: keep up to 12 distinct root PV moves seen while the
  candidate/reference searched.
- `--include-checks`, `--include-captures`, `--include-promotions`: on by
  default.
- `--max-moves-per-position 0`: no cap. Mandatory moves are never dropped.
- `--min-score-gap 0`: no thinning; raw output keeps scored alternatives.
- `--include-history-sensitive`: include rows where full-history replay already
  diverged from the logged game; off by default for static training targets.

## Options

- `--candidate`, `-c CANDIDATE`: candidate UCI engine executable. Default:
  `enyo`.
- `--reference`, `-r REFERENCE`: reference UCI engine executable. Default:
  `~/assets/engines/reference`.
- `--candidate-opts ARGS`: extra candidate process args; repeatable.
- `--reference-opts ARGS`: extra reference process args; repeatable.
- `--candidate-uci NAME=VAL`: extra candidate UCI option; repeatable.
- `--reference-uci NAME=VAL`: extra reference UCI option; repeatable.
- `--oracle <path>`: judge engine. Default: `stockfish`.
- `--oracle-opts <args>`: extra oracle process args.
- `--oracle-uci NAME=VAL`: extra oracle UCI option; repeatable.
- `--oracle-nodes N`: oracle node budget. Default: `200000`.
- `--oracle-depth N`: oracle depth budget; `0` follows logged depth.
- `--log`: analyze logged moves only; no candidate replay.
- `--jsonl`: write replay records to stdout.
- `--top-root-moves N`: JSONL root-PV alternative count.
- `--include-checks`, `--include-captures`, `--include-promotions`: JSONL
  tactical alternatives; enabled by default.
- `--no-checks`, `--no-captures`, `--no-promotions`: disable those JSONL
  tactical alternatives.
- `--max-moves-per-position N`: JSONL move cap after mandatory moves; `0`
  disables it.
- `--min-score-gap N`: optional JSONL post-score thinning.
- `--include-history-sensitive`: JSONL replay-divergence rows.
- `--no-analysis`: replay only; no oracle.
- `--moves`: print human per-position lines.
- `--move N`: start at fullmove `N`.
- `--count N`: limit analyzed logged engine moves.
- `--fixed-nodes N`: replay candidate with exactly `N` nodes. Default:
  `100000`.
- `--fixed-movetime [MS]`: replay candidate with fixed movetime.
- `--max-replay-nodes N`: use logged nodes capped at `N`; `0` disables cap.
- `--time`: replay with logged `go wtime/btime/...`.
- `--threads N`: send UCI `Threads`. Default: `1`.
- `--jobs N`: parallel batch jobs.
- `--color`: color human judgment labels.
- `--fen`: print FENs in analysis report lines.
- `--verbose`, `-v`: print UCI traffic and FENs.

The parser is deliberately pedantic: it rejects conflicting replay budgets,
rejects ignored candidate/reference/oracle combinations where correctness would
be ambiguous, and warns when an option is accepted but ineffective.
