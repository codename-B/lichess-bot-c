# lichess-bot-c

A C bridge between the [Lichess Bot API](https://lichess.org/api#tag/Bot) and chess engines.

## Features

- Lichess variants and time controls supported by your engine
- UCI, XBoard, and homemade engines
- Matchmaking
- Draw offers, resignations, and takebacks
- PGN files
- Local and online books and tablebases

Linux or Docker; WSL2 or Docker on Windows. No Python required.

## Steps

1. [Build](docs/SETUP.md#build)
2. [Create a token](docs/SETUP.md#create-a-token)
3. [Configure](docs/SETUP.md#configure-the-bot)
4. [Run](docs/SETUP.md#run)

## Advanced options

- [Configuration](docs/CONFIGURATION.md)
- [Homemade engines](docs/ENGINES.md#homemade-engines)
- [Custom handlers](docs/ENGINES.md#custom-game-handlers)
- [Docker](docs/SETUP.md#docker)
- [Contribute](docs/CONTRIBUTING.md)

## License

[AGPLv3 or later](LICENSE).

lichess-bot-c is adapted from [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot).
Thanks to its contributors and the Lichess team.
