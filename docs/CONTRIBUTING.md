# Contributing

Bug reports: include your environment and reproduction steps.

## Testing

[Build dependencies](SETUP.md#build).

```sh
make -j4 test
```

Tests cover modules; test bot/API sessions and adapter protocols separately.
[Fixtures](../tests/fixtures/SOURCES.md).

AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make sanitize-test
```
