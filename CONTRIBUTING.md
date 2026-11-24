# Contributing

Thanks for looking. This is a small, opinionated project; issues and pull requests are welcome.

- **Before a pull request:** build, and run the suites your change touches (README, "Build and test"). The core
  suites pass on Windows and in `--posix` mode; CI runs them on macOS.
- **The core stays pure.** `core/` includes no OS header and reaches the world only through the seams in
  `core/deck/seams.hpp`, so every rule can be tested on fakes.
- **A behaviour change comes with the check that shows it.**
- **No console windows.** Anything the deck starts runs hidden.
- **No secrets and no personal data** in code, fixtures or screenshots: synthetic names, `example.com`, temp paths.
- Keep the style of the file you are in.

By contributing you agree that your contribution is licensed under this repository's MIT licence.
