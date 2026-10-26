# Configuration

Options: [config.yml.default](../config.yml.default).

Use options and variants supported by your engine.

## C-specific settings

- Command-line options override YAML.
- `challenge.concurrency`: 0–16 engine processes; zero disables game workers.
- `c_bridge.runtime_directory`: directory for separately installed adapters.
- `state_directory`: automatic logs and takeback records.
- `matchmaking.start_immediately: true`: skip the initial idle delay.
- Extensions: [C plugins](ENGINES.md).

Omitted defaults differ from the example: `challenge.accept_bot: false`, `move_overhead: 1000` ms.

XBoard tablebases require `move_quality: best`; `suggest` requires UCI root-move restrictions.
