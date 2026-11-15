"""The source archive that goes with a release (DESIGN.md 16.1, GPL-3.0 section 6).

GitHub's own archives of a tag leave the submodules out. This one holds:

- the repository at HEAD (`git archive`, so only what Git tracks);
- every submodule at the commit HEAD pins, in its folder, except
  deps/SteamworkSDK: Valve's headers carry no licence and the submodule is
  fenced (DESIGN.md 16.5), so the archive names its repository and commit
  instead of repackaging it. Submodules of submodules are left out too: they
  are FidelityFX-FSR's sample program and its media, and RapidJSON's test
  framework, none of which the build reads;
- the RmlUi, FreeType and HarfBuzz source archives the launcher is built from,
  as downloaded (SOURCE.txt lists them with their SHA-256), in third_party/;
- SOURCE-ARCHIVE.txt, which says all this.

  python -I tools/release/source_archive.py --out dist/xerces-<version>-source.zip
      [--build-dir build] [--downloads DIR]

Refuses a tree with changes (the archive is of the commit). Reads Git only,
downloads nothing.
"""
import argparse
import hashlib
import pathlib
import subprocess
import sys
import tarfile
import time
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import notices  # noqa: E402

ROOT = notices.ROOT
LEFT_OUT = {"deps/SteamworkSDK": "Valve's Steamworks headers carry no licence; get them from the repository "
                                 "below at this commit (DESIGN.md 16.5)"}


def add_git_tree(zf, repo, commit, prefix, counts):
    """Adds `git archive <commit>` of repo under prefix. Symbolic links go in as small text files
    holding their target (a zip has no portable link)."""
    proc = subprocess.Popen(["git", "-C", str(repo), "archive", "--format=tar", commit], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=proc.stdout, mode="r|") as tar:
        for member in tar:
            name = prefix + member.name
            if member.isdir():
                continue
            if member.issym():
                zf.writestr(name, member.linkname)
                counts["links"] += 1
                continue
            if not member.isfile():
                continue
            data = tar.extractfile(member).read()
            info = zipfile.ZipInfo(name, date_time=tar_time(member.mtime))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (member.mode & 0o777) << 16
            zf.writestr(info, data)
            counts["files"] += 1
            counts["bytes"] += len(data)
    if proc.wait() != 0:
        raise notices.Problem(f"git archive {commit} in {repo} failed")


def tar_time(mtime):
    t = time.gmtime(max(mtime, 315532800))  # a zip cannot hold dates before 1980
    return t[:6]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", required=True)
    p.add_argument("--build-dir", default=str(ROOT / "build"))
    p.add_argument("--downloads", default=str(notices.DEFAULT_DOWNLOADS))
    p.add_argument("--allow-dirty", action="store_true", help="for a trial run: archive HEAD anyway")
    a = p.parse_args()
    out = pathlib.Path(a.out)
    try:
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        commit, dirty = notices.head()
        if dirty and a.allow_dirty:
            print("warning: the tree has changes; the archive holds the commit without them")
        elif dirty:
            raise notices.Problem("the tree has changes; the archive is of the commit: commit or stash first")
        subs = notices.submodules()
        tp = notices.third_party_dirs(pathlib.Path(a.build_dir))
        prefix = f"xerces-{version}-source/"
        counts = {"files": 0, "bytes": 0, "links": 0}
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix(".partial")
        included, left_out, archives = [], [], []
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            add_git_tree(zf, ROOT, commit, prefix, counts)
            for path in sorted(subs):
                sub_commit, describe, url = subs[path]
                if path in LEFT_OUT:
                    left_out.append(f"  {path}  {sub_commit}  {url}\n    {LEFT_OUT[path]}")
                    continue
                if not (ROOT / path / ".git").exists():
                    raise notices.Problem(f"{path} is not checked out: git submodule update --init {path}")
                add_git_tree(zf, ROOT / path, sub_commit, prefix + path + "/", counts)
                included.append(f"  {path}  {sub_commit}  {url}")
            for key in ("rmlui", "freetype", "harfbuzz"):
                if key not in tp:
                    continue
                name, licence, where = notices.ARCHIVES[key]
                ver = tp[key].name.split("-", 1)[1]
                folder, archive, url = where(ver)
                file = pathlib.Path(a.downloads) / folder / archive
                if not file.is_file():
                    raise notices.Problem(f"source archive missing: {file}")
                data = file.read_bytes()
                zf.writestr(f"{prefix}third_party/{archive}", data, compress_type=zipfile.ZIP_STORED)
                archives.append(f"  third_party/{archive}  {name} {ver} ({licence})\n"
                                f"    SHA-256 {hashlib.sha256(data).hexdigest()}, from {url}")
            note = "\n".join([
                f"SOURCE ARCHIVE OF XERCES {version}",
                "",
                f"The Xerces repository at commit {commit}",
                f"({notices.REPOSITORY}/tree/{commit}), with the submodules it builds with",
                "in their folders, and the source archives of the libraries the launcher is",
                "built from. Xerces is free software under the GNU General Public License,",
                "version 3 (LICENSE); each library keeps its own licence (its folder, and",
                "THIRD_PARTY_NOTICES.txt in the release).",
                "",
                "Submodules included (folder, commit, repository):",
                "",
                *included,
                "",
                "Left out:",
                "",
                *left_out,
                "  The submodules of deps/FidelityFX-FSR (sample/libs/Cauldron,",
                "  sample/media/Cauldron-Media) and of deps/rapidjson (thirdparty/gtest):",
                "  a sample program, its media and a test framework the build does not read.",
                "",
                "Library source archives (unpack them into one folder and pass it to premake",
                "with --xerces-third-party, as README.md, Building, says):",
                "",
                *archives,
                "",
                "To put the left-out Steamworks headers in place:",
                "",
                *[f"  git clone {subs[pth][2]} {pth}\n  git -C {pth} checkout {subs[pth][0]}"
                  for pth in sorted(LEFT_OUT) if pth in subs],
                "",
            ])
            zf.writestr(prefix + "SOURCE-ARCHIVE.txt", note.replace("\n", "\r\n"))
        tmp.replace(out)
        print(f"wrote {out}: {counts['files']:,} files ({counts['bytes'] / 2**20:.0f} MB unpacked), "
              f"{len(included)} submodules, {len(archives)} library archives, "
              f"{out.stat().st_size / 2**20:.0f} MB")
        if counts["links"]:
            print(f"  {counts['links']} symbolic links stored as text files holding their target")
    except notices.Problem as problem:
        print(f"source_archive: {problem}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
