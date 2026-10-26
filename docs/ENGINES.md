# Engines

Set `engine.protocol`: `uci`, `xboard`, or `homemade`.
Options: `engine.uci_options` or `engine.xboard_options` in [config.yml.default](../config.yml.default).

The bot manages `Ponder`, `UCI_Chess960`, `UCI_Variant`, and `MultiPV`.
XBoard engines must support CECP protocol 2, `ping`, `setboard`, and reuse after `new`.

## Homemade engines

Implement `search()` using [lbc_plugin.h](../include/lbc_plugin.h).

Set `engine.name` to `RandomMove`, `Alphabetical`, `FirstMove`, or `ComboEngine`,
or build a plugin:

```sh
cc -std=c11 -O3 -fPIC -shared -Iinclude \
  examples/homemade_first.c -o homemade-first.so
```

```yaml
engine:
  dir: .
  name: homemade-first.so
  protocol: homemade
```

Export `lbc_engine_plugin_v1()` returning an `LbcEnginePluginV1` descriptor.
The [example](../examples/homemade_first.c) chooses the alphabetically first UCI move.

## Custom game handlers

Build [custom_handlers.c](../examples/custom_handlers.c):

```sh
cc -std=c11 -O3 -fPIC -shared -Iinclude \
  examples/custom_handlers.c -o custom-handlers.so
```

```yaml
extra_game_handlers: ./custom-handlers.so
```

Export `lbc_hooks_plugin_v1()` returning an `LbcHooksPluginV1` descriptor.

- `is_supported_extra()`: return 1 to accept, 0 to reject.
- `game_specific_options()`: emit UCI/XBoard options through the sink; null means a button option.
- `event()`: account and game events.

Keep event-loop callbacks short. Call `host.submit` only from callbacks;
0 means queued, not delivered.
