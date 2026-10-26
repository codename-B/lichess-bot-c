# Setup

## Build

C11, Make, and libcurl 7.66.0+ with HTTPS. Debian/Ubuntu:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev
make -j4
```

Keep `lichess-bot-c`, `lbc-xboard`, and `lbc-homemade` together, or run
`sudo make install`. `make native` optimizes for your CPU in `build/native/`.
Windows: WSL2 or Docker Desktop with Linux containers.

macOS (experimental; requires Xcode command-line tools):

```sh
brew install curl pkg-config
export PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
make CC=clang DL_LIBS= CURL_CFLAGS="$(pkg-config --cflags libcurl)" \
  CURL_LIBS="$(pkg-config --libs libcurl)"
```

## Create a token

[Create a bot account](https://lichess.org/signup) and an
[OAuth token](https://lichess.org/account/oauth/token/create?scopes[]=bot:play&description=lichess-bot-c)
with `bot:play` scope. Save the token in `secrets/lichess_token` or set `LICHESS_BOT_TOKEN`.

## Configure the bot

```sh
cp config.yml.default config.yml
```

In `config.yml`, set:

- `engine.dir`: executable directory.
- `engine.name`: executable name.
- `engine.working_dir`: engine's working directory, if different.

[Configuration](CONFIGURATION.md) · [Engines](ENGINES.md)

## Run

```sh
./lichess-bot-c --config config.yml --token-file secrets/lichess_token
```

Add `--upgrade` to [convert to a BOT account](https://lichess.org/api#operation/botAccountUpgrade).
**Irreversible.**

Logging: `-v` for details, `--logfile FILE` for output, `--disable_auto_logging` to disable automatic logs.

Ctrl-C stops the bot. With `quit_after_all_games_finish: true`, it waits for
active games; a second Ctrl-C stops immediately.

## Docker

Put your Linux engine in `engines/`, token in `secrets/lichess_token`, and
config in `config.yml`. Set `engine.dir: /engines` and `engine.working_dir: /data`.
Books and tablebases mount at `/books` and `/tablebases`.

```sh
mkdir -p engines books tablebases secrets
docker compose up -d --build
docker compose logs -f
docker compose down
```

Runtime data uses a named volume. Source and licenses: `/usr/share/lichess-bot-c/source`.
