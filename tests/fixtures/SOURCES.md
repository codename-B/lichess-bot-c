# Test fixtures

Incomplete test tables. [Sources, sizes, hashes](tablebase_sources.json).

```sh
sha256sum -c tests/fixtures/SHA256SUMS
```

## Reference results

Generated with [python-chess 0c6bdac](https://github.com/niklasf/python-chess/tree/0c6bdaccfcb3b09cdbe94b0f81e948cb2a356015)
([license](../../vendor/python-chess-LICENSE)).

- `positions.tsv`: 297 positions, seed 123475. FEN, Polyglot hash, legal moves,
  selected move, SAN, resulting FEN.
- `tablebase_values.tsv`: 204 positions with WDL and DTZ.
- `tablebase_roots.tsv`: 276 root selections. Source: 1 = Syzygy, 2 = Gaviota;
  quality: 0 = best, 1 = suggest. Variant IDs follow [position.h](../../src/position.h).

## Reference adjustments

- Corrected python-chess's inverted `has_pawns` guard for pawnless `.stb` files.
- Counted checkmated children with Gaviota DTM zero as wins; preserved terminal
  wins at the fifty-move boundary.
- Skipped DTZ comparisons when missing promotion tables stopped the reference
  but the C reader found a winning zeroing move.

Update `SHA256SUMS` when changing fixture data.
