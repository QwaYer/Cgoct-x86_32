# 🐙 Cgoct/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C-orange.svg?style=for-the-badge" alt="Language: C">
  <img src="https://img.shields.io/badge/link-PIE%20%2B%20libc.so-purple.svg?style=for-the-badge" alt="PIE + libc.so">
  <img src="https://img.shields.io/badge/role-%2Fbin%2Finit-0369a1.svg?style=for-the-badge" alt="Role: /bin/init">
  <img src="https://img.shields.io/badge/output-cgoct-green.svg?style=for-the-badge" alt="cgoct">
</p>

<p align="center">
  A tiny <strong>ring-3 supervisor</strong> shipped as the first userspace program (<strong><code>/bin/init</code></strong> via <strong>cctkfs</strong>).<br>
  It opens <strong><code>/dev/tty</code></strong>, supervises <strong><code>/bin/cactsole</code></strong>, optional <strong><code>/bin/cactsole-rescue</code></strong> on crash-loops, and writes <strong><code>/etc/cgoct.conf</code></strong> / <strong><code>/var/log/cgoct.log</code></strong> when those paths exist.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Main binary** | `cgoct` (also staged as **`/bin/init`** in [`LocalRepoCactOS`](../LocalRepoCactOS)) |
| **Child programs** | `/bin/cactsole` (default), `/bin/cactsole-rescue --safe-mode` (rescue path) |
| **Load address** | PIE **ET_DYN** at **`0x08000000`** — same layout family as **cactsole** ([`link.ld`](link.ld)) |
| **libc** | Dynamic **`libc.so`** from **[CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32)** at **`0x10000000`** (see CactLib linker notes) |
| **Fast-crash window** | Child exit **≤ 3 s** with **non-zero** status counts toward burst detection |
| **Restart backoff** | On **fork/exec** failure: exponential delay **1 … 10 s** |
| **Config buffer** | **512** bytes max read from `/etc/cgoct.conf` |

---

## 🔗 Ecosystem

| Component | Role |
|-----------|------|
| **[CactKernel-x86_32](https://github.com/QwaYer/CactKernel-x86_32)** | Boots **`bin/init`** from **binfs** (ext4 + **cctkfs** overlay). That ELF is this supervisor. |
| **[CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32)** | **`libc.so`** + **`pic/start.o`** — required before linking **`cgoct`**. |
| **[Cactsole-x86_32](https://github.com/QwaYer/Cactsole-x86_32)** | Builds **`cactsole`**; **cactsole-rescue** is a second staged copy with a different argv (see LocalRepoCactOS Makefile). |
| **[LocalRepoCactOS](../LocalRepoCactOS)** | Packs **`cctkfs.img`**: copies **`cgoct`** → **`lib/bin/init`** and **`lib/bin/cgoct`**, plus **`cactsole`** binaries and **`libc.so`**. |

**Why a supervisor:** the kernel only launches **`init` once**. **cgoct** keeps the interactive shell (or rescue shell) alive under configurable restart policies and dampens crash-storms with cooldowns and optional rescue handoff.

---

## 🔨 Building

**Prerequisites** (same multilib story as the rest of the Cact userspace tree):

| Tool | Notes |
|------|-------|
| `gcc -m32` | Multilib **`gcc-multilib`** on amd64 hosts |
| `ld -m elf_i386` | GNU binutils, **`-pie --no-dynamic-linker`** |
| **`../CactLib-x86_32`** | Must build **`libc.so`** and **`build/pic/start.o`** first (the Makefile invokes this automatically) |

**Targets**

```sh
make -j"$(nproc)"   # produce ./cgoct
make clean          # remove objects and the binary
```

**Successful link** yields a single **`cgoct`** ELF next to this README; stage it with **`LocalRepoCactOS`** so the kernel sees it as **`/bin/init`**.

---

## 📂 Repository layout

```
Cgoct-x86_32/
├── Makefile          # gcc -m32, links start.o + main.o + libc.so
├── link.ld           # PIE layout @ 0x08000000 (matches cactsole family)
├── LICENSE           # GPLv3
├── src/
│   └── main.c        # supervisor loop, config parser, spawn/waitpid
└── README.md
```

---

## ⚙️ Configuration (`/etc/cgoct.conf`)

On first boot, if **`/etc/cgoct.conf`** is missing and the path is writable, **cgoct** seeds a comment block with defaults:

| Key | Values | Meaning |
|-----|--------|---------|
| **`restart_policy`** | `always` · `on-failure` · `once` | When to stop respawning the child |
| **`rescue_shell`** | `0` · `1` | After a crash-loop cooldown, try **`/bin/cactsole-rescue`** if present |
| **`crash_limit`** | `1` … `20` | Fast crashes within the window before cooldown |
| **`cooldown_sec`** | `1` … `120` | Sleep when a crash-loop is detected |

Lines starting with **`#`** are ignored. Unknown keys are skipped.

**Example**

```ini
restart_policy=always
rescue_shell=1
crash_limit=4
cooldown_sec=8
```

---

## 🔄 Supervisor loop (reference)

| Stage | Behaviour |
|-------|-----------|
| **Startup** | `prepare_files()` → `load_config()` → log file → **`/dev/tty`** on fds **0–2** |
| **Spawn** | `fork` + `execve` of **cactsole** or **cactsole-rescue** with **`PATH=/bin:/sbin`**, **`HOME=/`** |
| **Wait** | `waitpid`; on this kernel, **status is the raw exit code** (not POSIX-packed wait status bits) |
| **Clean exit** (`0`) | Resets fast-crash counter; with **`on-failure`**, supervisor **exits** |
| **Crash-loop** | If the child dies quickly **too many times**, sleep **`cooldown_sec`**, then prefer **rescue** on the next iteration when enabled |
| **Spawn failure** | Backoff up to **10 s**, set flag to try **rescue** next time |

---

## 🚀 Console output

Typical banner after boot:

```
cgoct: supervisor online
  restart policy : always
  rescue shell   : enabled
  crash limit    : 4
  cooldown       : 8 sec
```

Structured events are also appended to **`/var/log/cgoct.log`** when that file can be opened.
