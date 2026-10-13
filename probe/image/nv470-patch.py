#!/usr/bin/env python3
"""Make NVIDIA's 470.256.02 kernel modules (the last branch for Kepler GPUs) build on the stick's kernel (6.18,
gcc 15). NVIDIA no longer updates 470; without this, Kepler cards were only traced through nouveau.

  nv470-patch.py <extracted kernel/ dir>

Every change is anchored on exact text and asserted; a source that does not match is an error, never a partial patch.
What changes, and why:
  - Kbuild: Linux 6.15 stopped reading EXTRA_CFLAGS, so the include paths never reached the compiler (ccflags-y).
  - Kbuild: the feature probes ("conftest") are compiled with gcc 15's default C23, where bool/false are keywords, and
    without linux/kconfig.h; every probe failed and the driver took every old code path. They now use gnu11 + kconfig.h.
  - Kbuild: gcc 14+ makes an undeclared function an error, which turns NVIDIA's "does this function exist" probes
    upside down (a missing function looked present). Implicit declarations are allowed in the probes only.
  - Kbuild: del_timer_sync/del_timer were renamed timer_delete_sync/timer_delete in 6.15.
  - efi_enabled is a function taking EFI_BOOT; the old macro path returned the function itself.
  - follow_pfn() is gone and its replacement is GPL-only: report no PFN, as NVIDIA's own 580 driver does.
  - vm_flags_set()/vm_flags_clear() call a GPL-only symbol: write the flags word directly, as 580 does.
nvidia-drm is not built for 470 on this kernel (the DRM API moved too far); nvidia.ko brings the GPU up, which is what
the trace records, and the modeset step is skipped for these cards.
"""
import pathlib
import sys

EDITS = [
    ("Kbuild", "EXTRA_CFLAGS", "ccflags-y", None),
    ("Kbuild", "NV_CONFTEST_CFLAGS = $(NV_CFLAGS_FROM_CONFTEST) $(ccflags-y) -fno-pie\n",
     "NV_CONFTEST_CFLAGS = $(NV_CFLAGS_FROM_CONFTEST) $(ccflags-y) -fno-pie -std=gnu11 "
     "-include $(NV_KERNEL_SOURCES)/include/linux/kconfig.h "
     "-Wno-error=implicit-function-declaration -Wno-implicit-function-declaration\n", 1),
    ("Kbuild", "ccflags-y += -DNV_KERNEL_INTERFACE_LAYER\n",
     "ccflags-y += -DNV_KERNEL_INTERFACE_LAYER\n"
     "ifeq ($(shell grep -qs \"del_timer_sync\" $(NV_KERNEL_SOURCES)/include/linux/timer.h || echo gone),gone)\n"
     "ccflags-y += -Ddel_timer_sync=timer_delete_sync -Ddel_timer=timer_delete\n"
     "endif\n", 1),
    # 6.18's kbuild passes $(src) as a relative path, so the blob symlink pointed nowhere: link to the absolute path
    ("nvidia/nvidia.Kbuild", " cmd_symlink = ln -sf $< $@\n", " cmd_symlink = ln -sf $(abspath $<) $@\n", 1),
    ("nvidia-modeset/nvidia-modeset.Kbuild", "cmd_symlink = ln -sf $< $@\n", "cmd_symlink = ln -sf $(abspath $<) $@\n", 1),
    ("common/inc/nv-linux.h", "#define NV_EFI_ENABLED() efi_enabled\n", "#define NV_EFI_ENABLED() efi_enabled(EFI_BOOT)\n", 1),
    ("nvidia/os-mlock.c", "#if defined(NV_UNSAFE_FOLLOW_PFN_PRESENT)\n    return unsafe_follow_pfn(vma, address, pfn);",
     "#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)\n"
     "    /* follow_pfn() is gone and follow_pfnmap_start() is GPL-only: report no PFN, as NVIDIA 580 does */\n"
     "    return -1;\n"
     "#elif defined(NV_UNSAFE_FOLLOW_PFN_PRESENT)\n    return unsafe_follow_pfn(vma, address, pfn);", 1),
    ("common/inc/nv-mm.h", "static inline void nv_vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)\n{\n    vm_flags_set(vma, flags);\n}",
     "/* vm_flags_set() calls the GPL-only __vma_start_write(); like NVIDIA 580, write the word directly\n"
     " * (the mmap paths that call this already hold the mmap write lock) */\n"
     "static inline void nv_vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)\n{\n"
     "    ACCESS_PRIVATE(vma, __vm_flags) |= flags;\n}", 1),
    ("common/inc/nv-mm.h", "static inline void nv_vm_flags_clear(struct vm_area_struct *vma, vm_flags_t flags)\n{\n    vm_flags_clear(vma, flags);\n}",
     "static inline void nv_vm_flags_clear(struct vm_area_struct *vma, vm_flags_t flags)\n{\n"
     "    ACCESS_PRIVATE(vma, __vm_flags) &= ~flags;\n}", 1),
]


def main(root):
    root = pathlib.Path(root)
    mark = root / ".nv470-patched"
    if mark.exists():
        print("nv470-patch: already applied")
        return 0
    files = {}
    for rel, old, new, count in EDITS:
        s = files.get(rel) or (root / rel).read_text()
        n = s.count(old)
        if n == 0 or (count is not None and n != count):
            sys.exit("nv470-patch: %s: expected %s of %r, found %d" % (rel, count or "some", old[:60], n))
        files[rel] = s.replace(old, new)
    # a stale or broken blob link from an earlier build is removed; the (fixed) kbuild rule makes it again
    for d, blob in (("nvidia", "nv-kernel.o"), ("nvidia-modeset", "nv-modeset-kernel.o")):
        link = root / d / blob
        if link.is_symlink() or link.exists():
            link.unlink()
    for rel, s in files.items():
        (root / rel).write_text(s)
    mark.write_text("ok\n")
    print("nv470-patch: %d edits in %d files" % (len(EDITS), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
