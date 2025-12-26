# Fanorona

A Fanorona board game with a Java-based server and a web UI.

## Quick Start (Windows)

1. Download and extract the latest release.
2. Run `FanoronaAI.exe`.
3. The server will start in a console window, and your browser will open `http://localhost:8080` automatically.
4. **To Quit**: Close the console window. This ensures the AI memory is saved.

## Configuration

You can customize via command-line arguments:

| **Argument** | **Description**                                | **Default** |
| ------------ | ---------------------------------------------- | ----------- |
| `--time=N`   | Thinking time limit (ms)                       | 1000        |
| `--depth=N`  | Maximum search depth                           | 1000        |
| `--mem=N`    | Max memory entries                             | 3000000     |
| `--debug`    | Show detailed logs instead of trash talk in UI | Disabled    |

## License

MIT License. Developed at FAU Erlangen-Nürnberg.