from . import claude, codex, freebuff

MODS = {mod.MOD.name: mod.MOD for mod in (claude, codex, freebuff)}
DEFAULT = "claude"


def names() -> list[str]:
    return sorted(MODS)


def load(name: str):
    try:
        return MODS[name]
    except KeyError:
        raise SystemExit(f"unknown harness {name!r}; known: {', '.join(names())}")
