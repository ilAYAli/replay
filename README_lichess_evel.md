# Lichess Fishnet Evaluation

This documents the Lichess server-analysis path that `replay --log` tries to match locally.

## Fishnet Pipeline

Lichess does not analyze a game inside the web process. The `lila` server creates a `Work.Analysis` job and fishnet clients pick it up, run Stockfish, and post the evaluations back.

For normal user-requested game/study analysis, `lila` uses `Work.Origin.manualRequest`, which is `1,000,000` nodes per move. Other origins use different budgets:

- `officialBroadcast`: `5,000,000`
- `manualRequest`: `1,000,000`
- `autoHunter`: `300,000`
- `autoTutor`: `100,000`

The JSON sent to fishnet exposes that as the node budget for `sf18`, `sf17_1`, and `sf16`; classical engines get `nodes * 3`.

The fishnet client then applies its chunk-overlap adjustment before sending `go nodes` to Stockfish. Current fishnet chunks have `MAX_POSITIONS = 6`, so the effective UCI node budget is:

```text
1,000,000 * 6 / 7 = 857,142
```

That is why `replay` uses `857142` reference nodes by default when matching normal Lichess manual analysis.

Fishnet does not run a second confirmation pass for suspicious moves.

## Stockfish Setup

Fishnet sends one position at a time to Stockfish, using the game root position and the move list up to the analyzed ply. It prepares all game positions forward, reverses them, then analyzes them backwards in chunks. Each chunk starts with one overlap/warmup position and sends `ucinewgame` before the chunk. Keeping the move history matters; reducing the position to a bare FEN can change repetition and rule-state behavior.

For `replay --log`, the move list is reconstructed from the logged UCI `position ... moves ...` line. If the log starts after opening moves, those earlier plies are still in the move list and are included in the local side report. If the final logged move leaves the opponent with an immediate checkmate, replay appends one terminal mate so the reverse chunks match the completed Lichess game more closely.

The fishnet client source sets analysis options such as variant, MultiPV, and skill/move settings where relevant. It does not set a `SyzygyPath`, so local tablebases should not be assumed to match Lichess server analysis.

## Lichess Judgment Rules

Lichess stores one eval after each ply, then compares consecutive evals from the mover's point of view.

For centipawn evals:

```text
winning_chances(cp) = 2 / (1 + exp(-0.00368208 * cp)) - 1
```

The CP is capped to `[-1000, 1000]`. A move is judged by the mover's winning-chance loss:

- `>= 0.10`: inaccuracy
- `>= 0.20`: mistake
- `>= 0.30`: blunder

Mate transitions are handled separately. Creating an unavoidable mate against yourself is usually a blunder. Delaying an already forced mate is not repeatedly counted as a fresh blunder.

ACPL is computed from consecutive eval differences, CP-capped to `[-1000, 1000]`, from the player's point of view.

Game accuracy is not the average of displayed mistakes. Lichess combines a volatility-weighted mean with a harmonic mean over move accuracies.

## Local Differences

`replay` can be close to Lichess, but not guaranteed identical:

- Your local Stockfish build and NNUE files may differ from Lichess fishnet.
- Stockfish search is not perfectly stable across versions, hardware, hash state, and node budgets.
- Lichess exports stored analysis for old games; it may have been produced by an older Stockfish.
- Local tablebase settings can diverge from fishnet if enabled.
- Lichess may merge cached evals for positions fishnet skipped.

If the goal is to reproduce Lichess server analysis, use the default reference nodes and avoid local Syzygy settings. If the goal is a better current report, using a newer Stockfish is fine, but differences from the stored Lichess analysis are expected.

## Source Pointers

- `lila/modules/fishnet/src/main/Work.scala`: analysis origins and node budgets
- `lila/modules/fishnet/src/main/JsonApi.scala`: fishnet work JSON
- `fishnet/src/api.rs`: node-limit adjustment by chunk size
- `fishnet/src/ipc.rs`: `Chunk::MAX_POSITIONS`
- `fishnet/src/stockfish.rs`: Stockfish UCI setup and `go nodes`
- `lila/modules/tree/src/main/Advice.scala`: inaccuracy/mistake/blunder logic
- `scalachess/core/src/main/scala/eval.scala`: CP ceiling and winning-chance formula
- `lila/modules/analyse/src/main/AccuracyCP.scala`: ACPL
- `lila/modules/analyse/src/main/AccuracyPercent.scala`: move and game accuracy
