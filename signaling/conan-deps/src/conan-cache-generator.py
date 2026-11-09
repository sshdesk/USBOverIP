import argparse
import gzip
import hashlib
import json
import lzma
import logging
import os
import pathlib
import platform
import shutil
import subprocess
import sys
from datetime import datetime

import requests

# Lazy-import boto3 so Linux builds without S3 still work
g_s3 = None


class ConanException(Exception):
    pass


g_active_version = "v0.9"
g_is_windows = platform.system() == "Windows"

# ── Platform detection ─────────────────────────────────────────────
g_this_folder = pathlib.Path(__file__).parent.absolute()
g_tempdir = os.path.join(g_this_folder, "temp")
os.makedirs(g_tempdir, exist_ok=True)

g_archive_prefix = "conan-cache-msvc193" if g_is_windows else "conan-cache-linux"
g_7z_path = None
g_archive_ext = ".tar.gz"

if g_is_windows:
    g_7z_path = "C:\\Program Files\\7-Zip\\7z.exe"
    if not os.path.exists(g_7z_path):
        raise ConanException("7-Zip not found at default path")
    g_archive_ext = ".7z"

# ── Profiles ───────────────────────────────────────────────────────
g_profiles = {
    # Windows
    "windows_release": {
        "profile": os.path.join(g_this_folder, "profiles/windows_release_z7"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", "C:/cr/"),
        "cmake_config": "RelWithDebInfo",
    },
    "windows_minsizerel": {
        "profile": os.path.join(g_this_folder, "profiles/windows_minsizerel_z7"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", "C:/cm/"),
        "cmake_config": "MinSizeRel",
    },
    "windows_debug": {
        "profile": os.path.join(g_this_folder, "profiles/windows_debug_z7"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", "C:/cd/"),
        "cmake_config": "Debug",
    },
    # Linux
    "linux_release": {
        "profile": os.path.join(g_this_folder, "profiles/linux_release"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", os.path.expanduser("~/.conan2")),
        "cmake_config": "RelWithDebInfo",
    },
    "linux_debug": {
        "profile": os.path.join(g_this_folder, "profiles/linux_debug"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", os.path.expanduser("~/.conan2")),
        "cmake_config": "Debug",
    },
    "linux_relw": {
        "profile": os.path.join(g_this_folder, "profiles/linux_relw"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", os.path.expanduser("~/.conan2")),
        "cmake_config": "RelWithDebInfo",
    },
    "linux_dbg": {
        "profile": os.path.join(g_this_folder, "profiles/linux_dbg"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", os.path.expanduser("~/.conan2")),
        "cmake_config": "RelWithDebInfo",
    },
    "linux_debug_plain": {
        "profile": os.path.join(g_this_folder, "profiles/linux_debug_plain"),
        "conan_root": os.environ.get("CONAN_CACHE_ROOT", os.path.expanduser("~/.conan2")),
        "cmake_config": "Debug",
    },
}

g_build = "linux_release" if not g_is_windows else "windows_release"
g_github_token = os.environ.get("GITHUB_TOKEN", "")


# ── S3 (optional, lazy-load) ──────────────────────────────────────
def get_s3():
    global g_s3
    if g_s3 is None:
        try:
            import boto3
            import botocore
            from boto3.s3.transfer import S3Transfer
        except ImportError as e:
            raise ConanException(f"boto3 is required for S3 operations (pip install boto3): {e}")

        class S3:
            def __init__(self):
                self.client = boto3.client("s3")
                self.transfer = S3Transfer(self.client)

            def upload(self, filename, bucket, obj, content_type=None):
                extra_args = {"ContentType": content_type} if content_type else None
                self.transfer.upload_file(filename, bucket, obj, extra_args=extra_args)

            def upload_content(self, obj, bucket, content):
                temp_filename = os.path.join(g_tempdir, "s3.content")
                with open(temp_filename, "wb+") as f:
                    f.write(content)
                self.transfer.upload_file(temp_filename, bucket, obj)

            def download_content(self, obj, bucket, default_content=""):
                temp_filename = os.path.join(g_tempdir, "s3.content")
                try:
                    self.transfer.download_file(bucket, obj, temp_filename)
                except botocore.exceptions.ClientError as e:
                    if e.response["Error"]["Code"] != "404":
                        raise
                    return default_content
                with open(temp_filename, "rb") as f:
                    return f.read()

            def get_content_etag(self, obj, bucket, default_content=""):
                try:
                    resp = self.client.get_object(Bucket=bucket, Key=obj)
                except botocore.exceptions.ClientError as e:
                    if e.response["Error"]["Code"] in ("404", "NoSuchKey"):
                        return default_content, None
                    raise
                return resp["Body"].read(), resp.get("ETag")

            def put_content(self, obj, bucket, content, extra_args=None):
                self.client.put_object(Bucket=bucket, Key=obj, Body=content, **(extra_args or {}))

            def update_json(self, obj, bucket, mutate, retries=5):
                # Optimistic-concurrency read-modify-write: If-Match on ETag,
                # If-None-Match: * for create. Retries on PreconditionFailed.
                for attempt in range(retries):
                    content, etag = self.get_content_etag(obj, bucket, "")
                    j = json.loads(content) if content else {}
                    mutate(j)
                    extra = {"IfMatch": etag} if etag else {"IfNoneMatch": "*"}
                    try:
                        self.put_content(obj, bucket, json.dumps(j).encode("utf-8"), extra)
                        return j
                    except botocore.exceptions.ClientError as e:
                        if e.response["Error"]["Code"] != "PreconditionFailed" or attempt == retries - 1:
                            raise

        g_s3 = S3()
    return g_s3


# ── Helpers ────────────────────────────────────────────────────────
def find_files(root_dir, extension):
    found = []
    for root, _, files in os.walk(root_dir):
        for file in files:
            if file.endswith("." + extension):
                found.append(os.path.join(root, file))
    return found


def current_profile():
    return g_profiles[g_build]["profile"]


def current_conan_root():
    return g_profiles[g_build]["conan_root"]


def current_cmake_config():
    return g_profiles[g_build]["cmake_config"]


def conanfile_path():
    if g_is_windows:
        return os.path.join(g_this_folder, "conanfile.txt")
    return os.path.join(g_this_folder, "conanfile_linux.txt")


# ── Setup ──────────────────────────────────────────────────────────
def setup_conan_root():
    user_root = current_conan_root()
    os.makedirs(user_root, exist_ok=True)
    os.environ["CONAN_HOME"] = user_root


def setup_environment():
    if not g_github_token:
        os.environ["GITHUB_TOKEN"] = "github_cache" if os.environ.get("GITHUB_CACHE") else "nothing"


# ── OpenSSL patch (Windows PDB fix, skip on Linux) ─────────────────
def patch_conan_index_openssl():
    if not g_is_windows:
        return
    conan_index_path = os.path.join(g_this_folder, "..", "external", "conan-center-index")
    subprocess.run("git checkout .", cwd=conan_index_path, check=True)
    subprocess.run(
        "git apply --ignore-space-change ../patch/0001-create-pdb-file-to-make-openssl-happy.patch",
        cwd=conan_index_path,
        check=True,
    )


# ── Recipes ────────────────────────────────────────────────────────
def export_conan_recipes():
    # Common recipes
    paths = [
        ("conan-google-webrtc", ""),
       # ("firebasesdk", ""),
        ("speex", ""),
    ]

    # Windows-only: OpenSSL from conan-center-index + NVIDIA SDKs
    if g_is_windows:
        paths.insert(0, ("../external/conan-center-index/recipes/openssl/3.x.x/", "3.6.2"))
        paths.extend([
            ("capturesdk", ""),
            ("nvapi", ""),
            ("videocodecsdk", ""),
        ])

    for path, version in paths:
        fullpath = os.path.join(g_this_folder, path)
        if version:
            version = f"--version {version}"
        result = subprocess.run(f"conan export {version} {fullpath}", shell=True, check=True)
        if result.returncode != 0:
            print(f"BAD: {path}")
            sys.exit(-1)
        print(f"export {path}")


# ── Build ──────────────────────────────────────────────────────────
def build_step():
    profile = current_profile()
    cfile = conanfile_path()
    install_cmd = f"conan install {cfile} -of={g_tempdir} --profile:all={profile} --build=missing"
    print(f"Running: {install_cmd}")
    result = subprocess.run(install_cmd, shell=True, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout)
    if result.stderr:
        print(result.stderr)
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, install_cmd)


# ── Archive ────────────────────────────────────────────────────────
def pack(postfix=None):
    if postfix is None:
        clean_cmd = "conan cache clean -d -t -bs -b -s"
        subprocess.run(clean_cmd, shell=True, check=True)
        for lib in ["strawberryperl", "nasm", "cmake"]:
            subprocess.run(f"conan remove -c {lib}", shell=True, check=False)

    conan_root = current_conan_root()

    if postfix:
        filename = f"{g_archive_prefix}-{g_build}-{postfix}"
    else:
        filename = f"{g_archive_prefix}-{g_build}"

    if g_is_windows:
        target_path = os.path.join(g_tempdir, f"{filename}.7z")
    else:
        target_path = os.path.join(g_tempdir, f"{filename}.tar.gz")

    if os.path.exists(target_path):
        logging.info(f"target {target_path} already exists, removing...")
        os.unlink(target_path)

    if g_is_windows:
        compress_cmd = [g_7z_path, "a", target_path, conan_root + "/*"]
    else:
        compress_cmd = [
            "tar", "-czf", target_path,
            "-C", os.path.dirname(conan_root),
            os.path.basename(conan_root),
        ]
    subprocess.run(compress_cmd, check=True)
    print(f"Packed: {target_path}")


def unpack():
    conan_root = current_conan_root()

    if g_is_windows:
        target_path = os.path.join(g_tempdir, f"{g_archive_prefix}-{g_build}.7z")
    else:
        target_path = os.path.join(g_tempdir, f"{g_archive_prefix}-{g_build}.tar.gz")

    if os.path.exists(conan_root) and os.listdir(conan_root):
        logging.info(f"cache already unpacked: {conan_root}, skipping")
        return

    if os.path.exists(conan_root):
        shutil.rmtree(conan_root)

    os.makedirs(conan_root, exist_ok=True)

    if g_is_windows:
        decompress_cmd = [g_7z_path, "x", target_path, "-o" + conan_root]
    else:
        decompress_cmd = [
            "tar", "-xzf", target_path,
            "-C", os.path.dirname(conan_root),
        ]
    subprocess.run(decompress_cmd, check=True)
    print(f"Unpacked: {target_path} -> {conan_root}")


# ── GitHub releases ────────────────────────────────────────────────
def download_github_release(token, repo, version, filename, out_path):
    url = f"https://api.github.com/repos/{repo}/releases"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3.raw",
    }

    response = requests.get(url, headers=headers)
    if response.status_code == 401:
        raise ConanException("Failed to list releases, invalid GITHUB_TOKEN environment variable?")

    j = json.loads(response.text)
    download_url = ""
    for release in j:
        if release["tag_name"] == version:
            for asset in release["assets"]:
                if asset["name"] == filename:
                    download_url = asset["url"]

    headers = {
        "Authorization": "token {}".format(token),
        "Accept": "application/octet-stream",
    }

    response = requests.get(download_url, headers=headers)
    with open(out_path, "wb") as f:
        f.write(response.content)


def download():
    if g_is_windows:
        filename = f"{g_archive_prefix}-{g_build}.7z"
    else:
        filename = f"{g_archive_prefix}-{g_build}.tar.gz"
    out_path = os.path.join(g_tempdir, filename)
    if os.path.exists(out_path):
        logging.info(f"cache already downloaded: {out_path}, skipping")
        return
    conan_root = current_conan_root()
    if os.path.exists(conan_root) and os.listdir(conan_root):
        logging.info(f"cache already unpacked: {conan_root}, skipping download")
        return
    if not g_github_token:
        raise ConanException("GITHUB_TOKEN is required to download the cache")
    download_github_release(g_github_token, "real-app-platform/conan-deps", g_active_version, filename, out_path)


# ── Project build ──────────────────────────────────────────────────
def get_cmake_build_dir():
    root_name = "cache"
    current_dir = os.path.abspath(os.getcwd())

    if not os.path.exists(os.path.join(current_dir, "conanfile.py")):
        raise ConanException("Current working directory should contain conanfile.py")

    return os.path.join(current_dir, f"build-{root_name}-{g_build}")


def build_project(enable_ci=False):
    build_root = get_cmake_build_dir()
    os.makedirs(build_root, exist_ok=True)

    profile = current_profile()
    install_cmd = f"conan install .. -of {build_root} --profile:all={profile}"
    subprocess.run(install_cmd, shell=True, cwd=build_root, check=True)

    cmake_config = current_cmake_config()
    preset = f"conan-{cmake_config.lower()}"
    configure_preset = "conan-default" if g_is_windows else preset

    ci_flag = " -DCI_BUILD=ON -DENABLE_SENTRY=ON" if enable_ci else " -DCI_BUILD=OFF"
    asan_flag = " -DENABLE_ASAN=ON" if os.environ.get("ENABLE_ASAN") else ""
    cmake_cmd = f"cmake .. --preset {configure_preset}{ci_flag}{asan_flag}"
    subprocess.run(cmake_cmd, shell=True, cwd=build_root, check=True)

    if g_is_windows:
        cmake_cmd = f"cmake --build . --preset {preset} --target INSTALL"
    else:
        cmake_cmd = f"cmake --build . --preset {preset}"
    subprocess.run(cmake_cmd, shell=True, cwd=build_root, check=True)


def package_project():
    build_root = get_cmake_build_dir()
    logging.info(f"packaging project in {build_root}...")
    if not os.path.exists(build_root):
        raise ConanException(f"Buildroot {build_root} is not valid, did you build project first?")

    cmake_config = current_cmake_config()

    if g_is_windows:
        cmake_cmd = f"cmake --build . --config {cmake_config} --target package"
    else:
        cmake_cmd = "cmake --build . --target package"
    subprocess.run(cmake_cmd, shell=True, cwd=build_root, check=True)

    git_describe_cmd = "git describe --tags --long"
    git_version = subprocess.run(
        git_describe_cmd, cwd=build_root, check=True, shell=True, stdout=subprocess.PIPE
    ).stdout.decode("utf-8").splitlines()[0]

    time_now = datetime.now().strftime("%Y%m%d_%H%M%S")

    if g_is_windows:
        pdb_archive = os.path.join(
            build_root, "packages", f"{time_now}-{git_version}.pdbs.7z"
        )
        logging.info(f"compressing pdbs {pdb_archive}...")
        pdb_files = find_files(build_root, "pdb")
        if pdb_files:
            compress_cmd = [g_7z_path, "a", pdb_archive] + pdb_files
            subprocess.run(compress_cmd, check=True)

    # Linux: compress debug symbols (*.debug)
    if not g_is_windows:
        debug_archive = os.path.join(
            build_root, "packages", f"{time_now}-{git_version}.debug.tar.gz"
        )
        debug_files = find_files(build_root, "debug")
        if debug_files:
            logging.info(f"compressing debug symbols {debug_archive}...")
            compress_cmd = ["tar", "-czf", debug_archive] + debug_files
            subprocess.run(compress_cmd, check=True)


def upload_packages(bucket_name):
    s3 = get_s3()
    build_root = get_cmake_build_dir()
    packages_root = os.path.join(build_root, "packages")

    ext = "7z" if g_is_windows else "tar.gz"
    all_files = find_files(packages_root, ext)

    if g_is_windows:
        sym_files = find_files(packages_root, "pdbs.7z")
    else:
        sym_files = find_files(packages_root, "debug.tar.gz")

    bins = [f for f in all_files if f not in sym_files]

    if len(sym_files) > 1 or len(bins) > 1:
        raise ConanException("Packages folder is not clean, did you package multiple times without clean?")

    if not bins:
        raise ConanException(f"no bin packages found in {packages_root}, did you run --package-project?")

    if sym_files:
        logging.info(f"uploading {sym_files[0]} to {bucket_name}...")
        s3.upload(sym_files[0], bucket_name, "symbols/" + os.path.basename(sym_files[0]))

    filename = os.path.basename(bins[0])
    upload_path = "bins/" + filename
    logging.info(f"uploading {bins[0]} to {bucket_name}...")
    s3.upload(bins[0], bucket_name, upload_path)

    parts = filename.split("-", 2)
    name = parts[0]
    version = parts[1] if len(parts) > 1 else "unknown"
    logging.info(f"Package name {name}, version {version}")

    def update_index(j):
        j.setdefault("files", [])
        if upload_path not in j["files"]:
            j["files"].append(upload_path)
        j.setdefault("latest", {})
        j["latest"][name] = upload_path

    s3.update_json("index.json", bucket_name, update_index)


# ── PPA (apt repo on S3) ──────────────────────────────────────────
def hash_file(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_deb_control(deb_path):
    import io
    import tarfile

    ctrl_tar = subprocess.run(
        ["dpkg-deb", "--ctrl-tarfile", deb_path],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(ctrl_tar)) as tf:
        names = tf.getnames()
        control_name = "./control" if "./control" in names else "control"
        control_data = tf.extractfile(control_name).read().decode("utf-8")

    fields = {}
    current = None
    for line in control_data.splitlines():
        if line and line[0] == " " and current:
            fields[current] += " " + line.strip()
        elif ":" in line:
            current, value = line.split(":", 1)
            current = current.strip()
            fields[current] = value.strip()
    return fields


def render_packages(entries):
    blocks = []
    for e in sorted(entries, key=lambda x: (x["package"], x["version"])):
        block = [
            f"Package: {e['package']}",
            f"Version: {e['version']}",
            f"Architecture: {e['architecture']}",
            f"Maintainer: {e['maintainer']}",
            f"Filename: {e['pool']}",
            f"Size: {e['size']}",
            f"MD5Sum: {e['md5']}",
            f"SHA1: {e['sha1']}",
            f"SHA256: {e['sha256']}",
        ]
        if e.get("depends"):
            block.append(f"Depends: {e['depends']}")
        block.append(f"Section: {e.get('section', 'misc')}")
        block.append(f"Priority: {e.get('priority', 'optional')}")
        block.append(f"Description: {e.get('description', '')}")
        blocks.append("\n".join(block))
    return ("\n\n".join(blocks) + "\n").encode("utf-8")


def upload_ppa(bucket_name, deb_path, distro="noble", prefix="ppa"):
    s3 = get_s3()
    deb_path = os.path.abspath(deb_path)
    if not os.path.exists(deb_path):
        raise ConanException(f"deb file not found: {deb_path}")

    fields = read_deb_control(deb_path)
    for required in ("Package", "Version", "Architecture"):
        if required not in fields:
            raise ConanException(f"missing '{required}' in deb control")

    pkg = fields["Package"]
    version = fields["Version"]
    arch = fields["Architecture"]
    filename = os.path.basename(deb_path)
    rel_pool = f"pool/main/{pkg[0]}/{pkg}/{filename}"

    size = os.path.getsize(deb_path)
    logging.info(f"uploading {deb_path} -> s3://{bucket_name}/{prefix}/{rel_pool}")
    s3.upload(deb_path, bucket_name, f"{prefix}/{rel_pool}", content_type="application/vnd.debian.binary-package")

    # manifest.json is the source of truth for the apt index
    manifest_obj = f"{prefix}/manifest.json"
    entry = {
        "pool": rel_pool,
        "package": pkg,
        "version": version,
        "architecture": arch,
        "size": size,
        "md5": hash_file(deb_path, "md5"),
        "sha1": hash_file(deb_path, "sha1"),
        "sha256": hash_file(deb_path, "sha256"),
        "depends": fields.get("Depends", ""),
        "maintainer": fields.get("Maintainer", ""),
        "section": fields.get("Section", "misc"),
        "priority": fields.get("Priority", "optional"),
        "description": fields.get("Description", ""),
    }

    def update_manifest(j):
        j.setdefault("files", [])
        j["files"] = [f for f in j["files"] if f.get("pool") != rel_pool] + [entry]

    manifest = s3.update_json(manifest_obj, bucket_name, update_manifest)

    # render dists tree
    workdir = os.path.join(g_tempdir, "ppa")
    dists_dir = os.path.join(workdir, "dists", distro)
    arch_dir = os.path.join(dists_dir, "main", f"binary-{arch}")
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(arch_dir)

    packages = render_packages(manifest["files"])
    with open(os.path.join(arch_dir, "Packages"), "wb") as f:
        f.write(packages)
    with open(os.path.join(arch_dir, "Packages.gz"), "wb") as f:
        f.write(gzip.compress(packages))
    with open(os.path.join(arch_dir, "Packages.xz"), "wb") as f:
        f.write(lzma.compress(packages))

    release_path = os.path.join(dists_dir, "Release")
    with open(release_path, "w") as f:
        subprocess.run(["apt-ftparchive", "release", dists_dir], stdout=f, check=True)

    uploads = [
        (os.path.join(arch_dir, "Packages"), f"main/binary-{arch}/Packages", "text/plain"),
        (os.path.join(arch_dir, "Packages.gz"), f"main/binary-{arch}/Packages.gz", "application/gzip"),
        (os.path.join(arch_dir, "Packages.xz"), f"main/binary-{arch}/Packages.xz", "application/x-xz"),
        (release_path, "Release", "text/plain"),
    ]

    # optional GPG signing -> InRelease (otherwise clients need [trusted=yes])
    gpg_key = os.environ.get("PPA_GPG_KEY")
    if gpg_key:
        inrelease_path = os.path.join(dists_dir, "InRelease")
        subprocess.run(
            ["gpg", "--batch", "--yes", "--local-user", gpg_key, "--clearsign", "--output", inrelease_path, release_path],
            check=True,
        )
        uploads.append((inrelease_path, "InRelease", "text/plain"))

    for local, rel, ctype in uploads:
        s3.upload(local, bucket_name, f"{prefix}/dists/{distro}/{rel}", content_type=ctype)

    # upload public GPG key if not already present in S3
    key_local = os.path.join("ppa", "KEY.gpg")
    key_s3 = f"{prefix}/KEY.gpg"
    if os.path.exists(key_local):
        try:
            s3.client.head_object(Bucket=bucket_name, Key=key_s3)
            print(f"public key already in S3: s3://{bucket_name}/{key_s3}")
        except botocore.exceptions.ClientError as e:
            if e.response["Error"]["Code"] in ("404", "NoSuchKey"):
                s3.upload(key_local, bucket_name, key_s3, content_type="application/pgp-keys")
                print(f"public key uploaded: s3://{bucket_name}/{key_s3}")
            else:
                raise

    print(f"PPA updated: s3://{bucket_name}/{prefix}/")
    print(f"Client: deb [signed-by=KEY.gpg] https://{bucket_name}.s3.amazonaws.com/{prefix} {distro} main")


# ── CLI ────────────────────────────────────────────────────────────
def build_profile_choices():
    if g_is_windows:
        return ["windows_release", "windows_debug", "windows_minsizerel"]
    return ["linux_release", "linux_debug", "linux_relw", "linux_dbg", "linux_debug_plain"]


parser = argparse.ArgumentParser(description="Conan cache creator")
parser.add_argument("--build-cache", dest="build_cache", action="store_true", help="Build cache")
parser.add_argument(
    "--type",
    default=g_build,
    choices=build_profile_choices(),
    help="Build profile",
)
parser.add_argument("--pack", action="store_true", help="Pack cache files")
parser.add_argument("--download", action="store_true", help="Download cache files")
parser.add_argument("--unpack", action="store_true", help="Unpack cache files")
parser.add_argument(
    "--build-project",
    dest="build_project",
    action="store_true",
    help="Build project, conan file should be located in current dir.",
)
parser.add_argument(
    "--package-project",
    dest="package_project",
    action="store_true",
    help="Create packages, compress symbols.",
)
parser.add_argument(
    "--upload-packages",
    dest="upload_packages",
    choices=["stim-distfiles-dev", "stim-distfiles-staging", "stim-distfiles-prod"],
    help="Upload packages bucket.",
)
parser.add_argument(
    "--upload-ppa",
    dest="upload_ppa",
    choices=["stim-distfiles-dev", "stim-distfiles-staging", "stim-distfiles-prod"],
    help="Upload deb to PPA apt repo in bucket.",
)
parser.add_argument("--ci", action="store_true", help="CI/CD build: define NDEBUG, disable WebRTC DCHECKs, enable Sentry")
parser.add_argument("--deb", dest="deb_path", help="Path to .deb to upload (with --upload-ppa)")
parser.add_argument("--ppa-distro", dest="ppa_distro", default="noble", help="PPA distro codename (default noble)")
parser.add_argument("--ppa-prefix", dest="ppa_prefix", default="ppa", help="S3 prefix for PPA (default ppa)")

args = parser.parse_args()

g_build = args.type

setup_environment()
setup_conan_root()

if args.build_cache:
    patch_conan_index_openssl()
    export_conan_recipes()
    build_step()

    if args.pack:
        pack("full")

if args.pack:
    pack()

if args.download:
    download()

if args.unpack:
    unpack()

if args.build_project:
    build_project(enable_ci=args.ci)

if args.package_project:
    package_project()

if args.upload_packages:
    upload_packages(args.upload_packages)

if args.upload_ppa:
    if not args.deb_path:
        parser.error("--upload-ppa requires --deb")
    upload_ppa(args.upload_ppa, args.deb_path, args.ppa_distro, args.ppa_prefix)
