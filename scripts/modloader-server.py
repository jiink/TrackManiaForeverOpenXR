#!/usr/bin/env python3
"""Builds the TrackMania ModLoader (TMLoader) update server for TMFOXR.

A ModLoader server is a folder URL with an index.json (api 1) listing products and their
versions. For each version the ModLoader downloads `download_link`, checks the zip against
`hash_sha256`, unpacks it into database/TmForever/products/<product>/<version>/ and writes the
version entry next to it as description.yaml. So the zip it downloads holds only the version
folder's files, flat, without description.yaml.

This script reads this repository's GitHub releases, takes each one's
TMFOXR-<tag>-for-TMLoader.zip (see package-release.ps1), re-zips its version folder flat and
writes:

  <out>/index.json                         numbered releases (v7 and later)
  <out>/beta/index.json                    the same plus the current nightly build
  <out>/files/<version>/TMFOXR-modloader.zip

The workflow .github/workflows/modloader-server.yml publishes <out> on GitHub Pages, so people
add https://<owner>.github.io/<repo>/modloader/ (or .../modloader/beta/) under `servers:` in
%LOCALAPPDATA%\\TMLoader\\config.yaml.

  python scripts/modloader-server.py build --out-dir site/modloader
  python scripts/modloader-server.py check site/modloader/index.json site/modloader/beta/index.json

Standard library only.
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import tempfile
import urllib.request
import zipfile

DEFAULT_REPO = "jiink/TrackManiaForeverOpenXR"
PRODUCT = "TMFOXR"  # product folder name; install-tmloader.ps1 and the release zips use the same
GAME = "TmForever"
SERVED_ZIP = "TMFOXR-modloader.zip"
NIGHTLY_TAG = "nightly"
CHANGELOG_LIMIT = 4000
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$")
TAG = re.compile(r"^v(\d+)(?:\.(\d+))?(?:\.(\d+))?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
VERSION_KEYS = {"executable", "load_method", "priority", "dependencies", "download_link",
                "hash_sha256", "password", "changelog"}  # ProductVersionDescription in loader.d.ts
PRODUCT_KEYS = {"name", "author", "type", "external", "homepage", "description", "icon_link"}


# --- helpers ---------------------------------------------------------------------------------

def semver_key(version):
    m = SEMVER.match(version)
    if not m:
        return (0, 0, 0, 0, ())
    pre = m.group(4)
    ids = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")) if pre else ()
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if pre else 1, ids)


def tag_version(tag):
    """v9 -> 9.0.0, v9.1 -> 9.1.0 (the same rule as release.yml)."""
    m = TAG.match(tag)
    return f"{m.group(1)}.{m.group(2) or 0}.{m.group(3) or 0}" if m else None


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def http_get(url, accept="application/vnd.github+json"):
    req = urllib.request.Request(url, headers={"User-Agent": "tmfoxr-modloader-server", "Accept": accept})
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        req.add_header("Authorization", f"Bearer {token}")
    return urllib.request.urlopen(req, timeout=120)


def fetch_releases(repo):
    releases, url = [], f"https://api.github.com/repos/{repo}/releases?per_page=100"
    while url:
        with http_get(url) as r:
            releases += json.load(r)
            m = re.search(r'<([^>]+)>;\s*rel="next"', r.headers.get("Link") or "")
            url = m.group(1) if m else None
    return releases


def parse_flat_yaml(text):
    """The description.yaml files tmloader-description.ps1 writes: scalars plus one
    `dependencies:` list of {id, version}."""
    data, deps, in_deps = {}, [], False
    for line in text.splitlines():
        if not line.strip():
            continue
        if in_deps and line.startswith((" ", "\t")):
            m = re.match(r"^\s*(-\s*)?([a-z_]+):\s*(.*)$", line)
            if m:
                if m.group(1) or not deps:
                    deps.append({})
                deps[-1][m.group(2)] = m.group(3).strip().strip("'\"")
            continue
        in_deps = False
        m = re.match(r"^([a-z_]+):\s*(.*)$", line)
        if not m:
            continue
        key, value = m.group(1), m.group(2).strip()
        if key == "dependencies" and not value:
            in_deps = True
            data["dependencies"] = deps
        else:
            data[key] = value.strip("'\"")
    return data


def markdown_to_text(md, limit=CHANGELOG_LIMIT):
    """GitHub release notes -> the plain text the ModLoader shows."""
    out = []
    for line in md.replace("\r\n", "\n").splitlines():
        s = line.rstrip()
        if re.match(r"^\s*https://github\.com/user-attachments/\S+\s*$", s):  # embedded videos
            continue
        if re.match(r"^\s*<img\b.*>\s*$", s) or re.match(r"^\s*!\[.*\]\(.*\)\s*$", s):
            continue
        s = re.sub(r"^\s*>\s?", "", s)
        s = re.sub(r"^\[!(\w+)\]$", lambda m: m.group(1).capitalize() + ":", s)
        s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)
        s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
        heading = re.match(r"^#{1,6}\s+(.*)$", s)
        if heading and re.match(r"(?i)how[- ]?to\b|install", heading.group(1)):
            break  # installation steps do not belong in the ModLoader's changelog
        if heading:
            s = heading.group(1).rstrip(":") + ":"
        s = s.replace("**", "").replace("__", "").replace("`", "")
        s = re.sub(r"^(\s*)\* ", r"\1- ", s)
        out.append(s)
    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()
    if len(text) > limit:
        cut = text.rfind("\n", 0, limit)
        text = text[: cut if cut > 0 else limit].rstrip() + "\n..."
    return text


# --- release zip -> served zip ---------------------------------------------------------------

def read_release_zip(data):
    """-> (product description, version description, {file name: bytes}) from a
    TMFOXR-<tag>-for-TMLoader.zip: TMFOXR/description.yaml and TMFOXR/<any version>/*."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        product_yaml = f"{PRODUCT}/description.yaml"
        if product_yaml not in names:
            raise ValueError(f"no {product_yaml}")
        folders = {n.split("/")[1] for n in names if n.count("/") == 2 and n.startswith(PRODUCT + "/")}
        if len(folders) != 1:
            raise ValueError(f"expected one version folder, found {sorted(folders)}")
        folder = folders.pop()
        files = {}
        for n in names:
            parts = n.split("/")
            if len(parts) == 3 and parts[1] == folder and parts[2] != "description.yaml":
                files[parts[2]] = z.read(n)
        version_yaml = z.read(f"{PRODUCT}/{folder}/description.yaml").decode("utf-8")
        return (parse_flat_yaml(z.read(product_yaml).decode("utf-8")),
                parse_flat_yaml(version_yaml), files, folder)


def make_served_zip(files):
    """The version folder's files at the zip root with fixed timestamps: the same files give
    the same bytes, so rebuilding the site does not change any hash."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 0
            z.writestr(info, files[name], compresslevel=9)
    return buf.getvalue()


def download_asset(asset, cache_dir):
    url = asset["browser_download_url"]
    os.makedirs(cache_dir, exist_ok=True)
    listed = (asset.get("digest") or "").removeprefix("sha256:")
    # Cache by the listed digest only; the nightly asset is replaced in place under one URL.
    path = os.path.join(cache_dir, listed + ".zip") if listed else None
    if path and os.path.exists(path):
        with open(path, "rb") as f:
            return f.read()
    with http_get(url, accept="application/octet-stream") as r:
        data = r.read()
    if listed and sha256_bytes(data) != listed:
        raise SystemExit(f"{url}: sha256 {sha256_bytes(data)} != listed {listed}")
    if path:
        with open(path, "wb") as f:
            f.write(data)
    return data


# --- build -----------------------------------------------------------------------------------

def build(releases, out_dir, base_url, repo, cache_dir):
    homepage = f"https://github.com/{repo}"
    entries = []  # (version, is_nightly, entry)
    product_meta = None
    for rel in releases:
        tag = rel.get("tag_name", "")
        if rel.get("draft"):
            continue
        is_nightly = tag == NIGHTLY_TAG
        if not is_nightly and not tag_version(tag):
            continue
        asset = next((a for a in rel.get("assets", [])
                      if a.get("name") == f"TMFOXR-{tag}-for-TMLoader.zip"), None)
        if not asset:
            print(f"skip {tag}: no TMLoader zip")
            continue
        product, version_desc, files, folder = read_release_zip(download_asset(asset, cache_dir))
        # Numbered releases take their version from the tag (v7 and v8 shipped a 0.1.0-dev
        # folder); the nightly from its folder (<next major>.0.0-dev.<run>).
        version = folder if is_nightly else tag_version(tag)
        if not SEMVER.match(version):
            print(f"skip {tag}: version {version!r} is not semver")
            continue
        executable = version_desc.get("executable")
        if executable not in files:
            raise SystemExit(f"{tag}: {executable!r} not in the version folder ({sorted(files)})")
        served = make_served_zip(files)
        rel_path = f"files/{version}/{SERVED_ZIP}"
        target = os.path.join(out_dir, *rel_path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as f:
            f.write(served)

        notes = "" if is_nightly else markdown_to_text(rel.get("body") or "")
        changelog = notes or version_desc.get("changelog", "")
        link = rel.get("html_url") or f"{homepage}/releases/tag/{tag}"
        entry = {"executable": executable}
        if version_desc.get("priority"):
            entry["priority"] = int(version_desc["priority"])
        entry["dependencies"] = [{k: d[k] for k in ("id", "version") if k in d}
                                 for d in version_desc.get("dependencies", [])]
        entry["download_link"] = f"{base_url}/{rel_path}"
        entry["hash_sha256"] = sha256_bytes(served)
        entry["changelog"] = (changelog + "\n\n" if changelog else "") + "Release notes: " + link
        entries.append((version, is_nightly, entry))
        if not is_nightly and (product_meta is None or semver_key(version) > product_meta[0]):
            product_meta = (semver_key(version), product)
        print(f"{tag} -> {version} ({len(served)} bytes, sha256 {entry['hash_sha256']})")

    if product_meta is None:
        raise SystemExit("no numbered release with a TMLoader zip")
    entries.sort(key=lambda e: semver_key(e[0]))
    product = {k: v for k, v in product_meta[1].items() if k in PRODUCT_KEYS}
    product.setdefault("homepage", homepage)

    def index(with_nightly):
        versions = {v: e for v, nightly, e in entries if with_nightly or not nightly}
        return {"api": 1, "game": GAME, "products": {PRODUCT: dict(product, versions=versions)}}

    for rel_path, data in (("index.json", index(False)), ("beta/index.json", index(True))):
        path = os.path.join(out_dir, *rel_path.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=2, ensure_ascii=True)
            f.write("\n")
        print(f"{rel_path}: {', '.join(data['products'][PRODUCT]['versions'])}")


# --- check -----------------------------------------------------------------------------------

def check(paths, out_dir, base_url):
    errors = []
    for path in paths:
        def err(msg):
            errors.append(f"{path}: {msg}")
        data = json.load(open(path, encoding="utf-8"))
        if data.get("api") != 1 or data.get("game") != GAME or set(data) != {"api", "game", "products"}:
            err("top level must be exactly {api: 1, game: TmForever, products}")
        for pid, prod in (data.get("products") or {}).items():
            for k in ("name", "author", "type", "versions"):
                if k not in prod:
                    err(f"{pid}: missing {k}")
            if set(prod) - {"versions"} - PRODUCT_KEYS:
                err(f"{pid}: fields TMLoader does not know: {sorted(set(prod) - {'versions'} - PRODUCT_KEYS)}")
            if not prod.get("versions"):
                err(f"{pid}: no versions")
            for ver, e in (prod.get("versions") or {}).items():
                where = f"{pid} {ver}"
                if not SEMVER.match(ver):
                    err(f"{where}: not semver")
                if set(e) - VERSION_KEYS:
                    err(f"{where}: fields TMLoader does not know: {sorted(set(e) - VERSION_KEYS)}")
                for k in ("executable", "dependencies", "download_link", "hash_sha256"):
                    if k not in e:
                        err(f"{where}: missing {k}")
                if not SHA256.match(str(e.get("hash_sha256", ""))):
                    err(f"{where}: hash_sha256 is not 64 lowercase hex digits")
                link = str(e.get("download_link", ""))
                if not link.startswith("https://"):
                    err(f"{where}: download_link is not https")
                if out_dir and base_url and link.startswith(base_url + "/"):
                    local = os.path.join(out_dir, *link[len(base_url) + 1:].split("/"))
                    if not os.path.exists(local):
                        err(f"{where}: {local} missing")
                        continue
                    blob = open(local, "rb").read()
                    if sha256_bytes(blob) != e.get("hash_sha256"):
                        err(f"{where}: served zip hash differs from hash_sha256")
                    with zipfile.ZipFile(io.BytesIO(blob)) as z:
                        names = z.namelist()
                        if z.testzip() is not None:
                            err(f"{where}: zip is corrupt")
                    if e.get("executable") not in names:
                        err(f"{where}: {e.get('executable')} not at the zip root")
                    if any("/" in n for n in names) or "description.yaml" in names:
                        err(f"{where}: zip must be flat and without description.yaml ({names})")
    for e in errors:
        print("ERROR:", e)
    print("OK" if not errors else f"{len(errors)} error(s)")
    return 1 if errors else 0


def default_base_url(repo):
    owner, name = repo.split("/", 1)
    return f"https://{owner.lower()}.github.io/{name}/modloader"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or DEFAULT_REPO)
    ap.add_argument("--base-url", help="URL the out dir is served at (default https://<owner>.github.io/<repo>/modloader)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("build")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--releases", help="releases JSON in GitHub API format (default: fetch)")
    p.add_argument("--cache", default=os.path.join(tempfile.gettempdir(), "tmfoxr-modloader-cache"))
    p = sub.add_parser("check")
    p.add_argument("index", nargs="+")
    p.add_argument("--out-dir", help="also verify the served zips in this folder")
    args = ap.parse_args()
    base_url = (args.base_url or default_base_url(args.repo)).rstrip("/")
    if args.cmd == "build":
        releases = json.load(open(args.releases, encoding="utf-8")) if args.releases else fetch_releases(args.repo)
        build(releases, args.out_dir, base_url, args.repo, args.cache)
        return 0
    return check(args.index, args.out_dir, base_url)


if __name__ == "__main__":
    sys.exit(main())
