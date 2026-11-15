"""
PlatformIO pre-build script: apply CrossPoint's JPEGDEC patches via `git apply`.

The upstream JPEGDEC pin still has progressive grayscale bugs in
JPEGDecodeMCU_P, including unsafe MCU_SKIP writes and attempts to consume
chroma components that are absent from the current scan.
The patches in `scripts/jpegdec_patches/` carry the fix; this script applies
each one against the libdep working tree.

Each patch's idempotency is decided by git itself, from the last patch backwards:
patches later in the list may touch the same hunk as an earlier one, so the
reverse check of an earlier patch no longer matches once a later one is applied.
  * find the highest patch that reverse-applies  -> everything up to it is applied
  * forward-apply the remaining patches in lexical order
  * neither direction works for a remaining patch -> abort the build

Patches live in `scripts/jpegdec_patches/` as one-commit-per-fix files
(see the file headers for context). Applied in lexical order.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "jpegdec_patches")  # noqa: F821


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    for env_dir in os.listdir(libdeps_dir):
        jpeg_dir = os.path.join(libdeps_dir, env_dir, "JPEGDEC")
        if not os.path.isdir(os.path.join(jpeg_dir, ".git")):
            continue
        applied = _applied_prefix(jpeg_dir, patches)
        for patch in patches[applied:]:
            _apply_one(jpeg_dir, patch)


def _applied_prefix(jpeg_dir, patches):
    """Number of patches already applied, found from the last one backwards."""
    for index in range(len(patches) - 1, -1, -1):
        if _git_apply_succeeds(jpeg_dir, patches[index], reverse=True):
            return index + 1
    return 0


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_one(jpeg_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(jpeg_dir, patch_path, reverse=True):
        return
    if not _git_apply_succeeds(jpeg_dir, patch_path, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=jpeg_dir,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(
            "ERROR: JPEGDEC patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=jpeg_dir, check=True)
    print("Applied JPEGDEC patch: %s" % name)


def _git_apply_succeeds(jpeg_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(
        cmd, cwd=jpeg_dir, capture_output=True, text=True
    ).returncode == 0


patch_jpegdec(env)  # noqa: F821
