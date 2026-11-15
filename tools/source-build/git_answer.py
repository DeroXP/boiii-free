"""Stands in for git when a release's source archive is built (it has no .git).

tools/release/source_archive.py records SOURCE_GIT.json at the archive's root: the output of every "git ..."
command premake5.lua runs, as the release commit gave them. tools/build.ps1 puts this folder first on PATH
when there is no .git, so premake's `git ...` runs git.cmd here, which prints the recorded answer (nothing,
and exit code 1, for a command that was not recorded). The programs built so name the release commit.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
answers = json.loads((ROOT / "SOURCE_GIT.json").read_text(encoding="utf-8"))
command = " ".join(["git"] + sys.argv[1:])
if command not in answers:
    sys.exit(1)
sys.stdout.write(answers[command])
