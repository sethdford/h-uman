# Never `cp` Over a Running Binary on macOS

Replacing a running binary at the same path via `cp` truncates-and-rewrites
the existing inode. macOS dyld may still be loading pages from that inode
for in-flight invocations, and the partial-overwrite produces an
inconsistent mmap view. The affected processes hang indefinitely in
`_dyld_start`, **uninterruptible by SIGKILL** (process stat = `UE`), and
the only way to clear them is a reboot.

## The hazard

Concrete repro from 2026-05-27 trusting-tharp session:

```bash
cp /Users/sethford/Projects/h-uman/build/human \
   /Users/sethford/.local/bin/human-daemon

nohup /Users/sethford/.local/bin/human-daemon service-loop --with-gateway &
# PID hangs in dyld, log file stays 0 bytes, SIGKILL fails to terminate
```

Five processes accumulated this way over ~20 minutes of debugging. All
five referenced the SAME inode (112926941). All five had ps stat `UE`
(uninterruptible + exiting). The OS refused to reap them; only the
mac itself rebooting would release them.

Even `human-daemon --version` invocations hit the same path and hung,
because they map the same inode whose pages are now poisoned. Every
follow-up invocation added another zombie to the pile.

## Why it happens

- `cp old new` opens `new` with `O_TRUNC` then writes the source bytes
- `new` is the same inode that was mmap'd by running processes
- macOS dyld's binary-loading involves multiple mmap'd regions
  (__TEXT, __DATA, __LINKEDIT, cdhash signature page)
- An in-flight load that's already mmap'd `__TEXT` but hasn't yet
  mapped `__LINKEDIT` will see TWO different versions of the file
  when truncate happens between those operations
- The dyld guard against this is to wait — indefinitely

## The right fix

**Always use atomic mv from a staged temp file** for any "install"
operation. The pattern:

```bash
cp source target.staged-$$
chmod 0755 target.staged-$$
codesign --force --sign "..." --identifier "..." target.staged-$$  # if relevant
mv -f target.staged-$$ target
```

Why this works:
- `mv` on the same filesystem is an atomic rename at the inode level
- The OLD inode keeps its identity — in-flight processes keep loading
  from it and finish normally (or, if they were never going to load,
  stay in their existing partial state without becoming worse)
- The NEW inode is what `target` now points to
- New invocations of `target` see the new inode from open() onward —
  no overlap, no partial view

## Don't reinvent — use the project's install script

`h-uman` ships the canonical implementation at:

```bash
scripts/install-human-daemon.sh
```

It does:
1. Stage to `$INSTALL_BIN.staged-$$`
2. Re-sign at the install path with a stable identifier
3. `mv -f` atomically
4. `launchctl bootout` the old service + `launchctl bootstrap` the new
5. Verify with doctor

**Use it whenever you need to update `~/.local/bin/human-daemon`.**

## When this rule applies

Apply for ANY install/update of a binary that's potentially:
- Currently running
- Mapped into a long-lived process's address space
- Referenced by launchd / systemd / Windows Services
- Loadable as a dyld dependency

Even if you "know" no process is using it right now, use atomic mv —
it's the same syscall cost as `cp`, but resilient.

## When this rule does NOT apply

- The binary path is brand new (file doesn't exist yet) — `cp` is fine
- Installing on a filesystem that doesn't support atomic rename
  (cross-device, smbfs, etc.) — `cp` is the only option, but then
  you must ensure no process is using the target
- One-off shell scripts that you control entirely

## Detection signals

If you ever see:
- Multiple `human-daemon` PIDs in `ps` with stat `UE` (or any uninterruptible
  state) that won't die from SIGKILL
- `nohup binary &` produces a PID but a `> log 2>&1` log stays 0 bytes
- `sample <pid>` shows the process hung at `_dyld_start` with no user
  frames

Then you've already triggered this hazard. Recovery:
- The zombies will not clear without reboot
- The CURRENTLY-stuck path can sometimes be salvaged by atomic mv'ing
  a different binary in (new inode means new processes work fine)
- Future installs MUST use atomic mv

## Related

- `~/.claude/rules/cmake-build-stale-binary.md` — the OTHER way the
  install can silently fail (rebuilt source but the linker didn't
  relink the executable). Compose: `touch source && cmake --build` →
  `cmake build/human` → `scripts/install-human-daemon.sh` (NOT plain
  `cp`).
- `scripts/install-human-daemon.sh` — the canonical correct install
- macOS man page: `dyld(1)` for `_dyld_shared_cache_path` and the
  signature-validation flow
