# MIT License - LugalOS MicroPython mip package installer
import json
import os
import sys
import urequests

_DEFAULT_INDEX = "https://micropython.org/pi/v2"


def _default_target():
    try:
        os.stat("/nvme0")
        return "/nvme0/lib/python"
    except OSError:
        return "/sd0/lib/python"


def _makedirs(path):
    parts = path.split("/")
    cur = ""
    for p in parts:
        if not p:
            cur = "/"
            continue
        cur = (cur + p) if cur.endswith("/") else (cur + "/" + p)
        try:
            os.stat(cur)
        except OSError:
            os.mkdir(cur)


def _rewrite_url(url, branch="HEAD"):
    if url.startswith("github:"):
        rest = url[7:]
        parts = rest.split("/", 2)
        if len(parts) < 2:
            raise ValueError("invalid github: url")
        owner, repo = parts[0], parts[1]
        sub = parts[2] if len(parts) > 2 else "package.json"
        return "https://raw.githubusercontent.com/{}/{}/{}/{}".format(owner, repo, branch, sub)
    if url.startswith("gitlab:"):
        rest = url[7:]
        parts = rest.split("/", 2)
        if len(parts) < 2:
            raise ValueError("invalid gitlab: url")
        owner, repo = parts[0], parts[1]
        sub = parts[2] if len(parts) > 2 else "package.json"
        return "https://gitlab.com/{}/{}/-/raw/{}/{}".format(owner, repo, branch, sub)
    return url


def _download_file(url, dest):
    print("Installing", url, "->", dest)
    resp = urequests.get(url)
    if resp.status_code != 200:
        raise OSError("HTTP status {}".format(resp.status_code))
    slash = dest.rfind("/")
    if slash > 0:
        _makedirs(dest[:slash])
    with open(dest, "wb") as f:
        f.write(resp.content)
    resp.close()


def _install_json(package_json_url, index, target, version):
    resp = urequests.get(_rewrite_url(package_json_url, version or "HEAD"))
    if resp.status_code != 200:
        raise OSError("HTTP status {} for {}".format(resp.status_code, package_json_url))
    data = resp.json()
    resp.close()

    for dep, dep_ver in data.get("deps", []):
        install(dep, index=index, target=target, version=dep_ver)

    for target_path, src_url in data.get("urls", []):
        full_dest = target + "/" + target_path.lstrip("/")
        _download_file(_rewrite_url(src_url, version or "HEAD"), full_dest)


def install(package, index=None, target=None, version=None):
    if target is None:
        target = _default_target()
    if index is None:
        index = _DEFAULT_INDEX
    _makedirs(target)

    if package.endswith(".py") or package.endswith(".mpy"):
        url = _rewrite_url(package, version or "HEAD")
        name = url.rsplit("/", 1)[-1]
        _download_file(url, target + "/" + name)
        return

    if package.startswith("http://") or package.startswith("https://") or package.startswith("github:") or package.startswith("gitlab:"):
        _install_json(package, index, target, version)
        return

    ver = version or "latest"
    manifest_url = "{}/package/6/{}/{}.json".format(index.rstrip("/"), package, ver)
    _install_json(manifest_url, index, target, version)


def main():
    args = sys.argv[1:]
    if not args or args[0] != "install":
        print("usage: mpy -m mip install <package> [--target <dir>]")
        return
    target = None
    pkgs = []
    i = 1
    while i < len(args):
        if args[i] == "--target" and i + 1 < len(args):
            target = args[i + 1]
            i += 2
        else:
            pkgs.append(args[i])
            i += 1
    for p in pkgs:
        install(p, target=target)


if __name__ == "__main__":
    main()
