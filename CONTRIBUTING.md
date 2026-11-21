# Contributing to Ambiance

Thank you for wanting to help. Ambiance is one project with one release,
and improvements are best made here, where everyone who uses it gets them.

## How

- **Bugs and ideas:** open an issue. Say which Amiga (CPU, chipset or
  graphics card, Kickstart and Workbench), which screen mode or window, and
  what you did. The log helps most: turn on Settings > Debug and attach
  `Ambiance.log` from Ambiance's drawer.
- **Changes:** open a pull request against `main`.
  - Build and test first: `make` and `make test` (see "Building" in the
    README). CI runs on pull requests too.
  - Keep a pull request to one change, and describe what it changes for
    someone using Ambiance.
  - New text shown on screen goes through `_()`; run
    `python3 tools/catalog.py scan src catalogs` so the catalogs know of it.
    Translations of new strings are welcome but not required.
- **Translations:** the catalogs are in `catalogs/` (one `.ct` file a
  language). Corrections are welcome.

## Terms

Contributions are accepted under the project's licence, the MIT licence
(LICENSE): by opening a pull request you agree that your change may be
released under it, as part of Ambiance.

## The name

The licence covers the code, not the name. "Ambiance" and its logo identify
this project's own releases. You are free to fork the code under the
licence, but a modified version you release yourself must use another name
and logo, and must not suggest it is Ambiance or comes from this project.
