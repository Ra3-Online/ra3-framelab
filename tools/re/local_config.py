"""Local inputs for optional reverse-engineering tools; no game files are bundled.

FLAB_IMAGE overrides FLAB_GAME_ROOT/Data/ra3_1.12.game.
FLAB_IDA_DUMP is optional for symbol annotations, required for dump-only tools.
Legacy positional arguments remain unchanged. Tools that used implicit inputs also
accept --image PATH / --ida-dump PATH, removed from argv by take_input_options().
"""

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def build_directory():
    directory = Path(os.environ.get("FLAB_BUILD_DIR") or REPO_ROOT / "build").expanduser()
    if not directory.is_absolute():
        directory = REPO_ROOT / directory
    return directory.resolve()


def take_input_options(args):
    rest, options = [], {}
    iterator = iter(args)
    for arg in iterator:
        if arg in ("--image", "--ida-dump"):
            try:
                options[arg] = next(iterator)
            except StopIteration:
                raise SystemExit(f"{arg} requires a path")
        else:
            rest.append(arg)
    return rest, options


def input_file(path, description):
    if not path:
        raise SystemExit(f"{description} is not configured")
    result = Path(path).expanduser().resolve()
    if not result.is_file():
        raise SystemExit(f"{description} not found: {result}")
    return result


def image_path(override=None):
    path = override or os.environ.get("FLAB_IMAGE")
    if not path and os.environ.get("FLAB_GAME_ROOT"):
        path = Path(os.environ["FLAB_GAME_ROOT"]) / "Data" / "ra3_1.12.game"
    return input_file(path, "RA3 image (--image / FLAB_IMAGE / FLAB_GAME_ROOT)")


def ida_dump_path(override=None, required=False):
    path = override or os.environ.get("FLAB_IDA_DUMP")
    if not path and not required:
        return None
    return input_file(path, "IDA decompilation (--ida-dump / FLAB_IDA_DUMP)")
