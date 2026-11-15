# Building and running on QNX 8.0

This guide takes the project from a fresh machine to the guardian running on a QNX 8.0 target. Everything uses free, non-commercial QNX Everywhere tools.

## 1. Get the free license (one time)

1. Create a **myQNX account** at qnx.com, in your own name.
2. While logged in, open **QNX Everywhere** (qnx.software/en/developers/get-started/qnx-everywhere) and fill in the license form. It's free, and the license is for non-commercial use only.
3. Wait for the license to show up in your myQNX profile, under Registered Products. QNX says this can take up to an hour.
4. Download and install **QNX Software Center** and sign in with the same account.
5. In Software Center, install:
   - **QNX Software Development Platform 8.0**, which lands in `~/qnx800` by default
   - the **QNX SDP 8.0 quick start target image for QEMU** (`com.qnx.qnx800.quickstart.qemu`), if you'll use QEMU
6. Optionally, install the **QNX Toolkit** extension for VS Code.

## 2. Choose a target

| Host | Target | Notes |
|---|---|---|
| Ubuntu | QEMU x86_64 | `mkqnximage` supports QEMU on Ubuntu hosts. The quick start image unpacks under `~/qnx800/images/qemu`. |
| Windows | VMware Workstation Player, x86_64 | `mkqnximage --type=vmware`, or the QNX Toolkit "vm > vmware" target in VS Code |
| Any | Raspberry Pi 5 | QNX's ready-made Pi image. Build with `QNX_ARCH=aarch64le`. |

**QEMU quick start** (from QNX's Getting Started page):

```sh
cd ~/qnx800/images/qemu && ./unpack_qemu_image.sh
source ~/qnx800/qnxsdp-env.sh
cd ~/qnx800/images/qemu/qemu && mkqnximage --run      # the terminal becomes the serial console
mkqnximage --getip                                     # from another terminal, same folder
```

## 3. Build

```sh
source ~/qnx800/qnxsdp-env.sh
./qnx/build-qnx.sh                       # x86_64, native message passing -> build-qnx-x86_64/
QNX_ARCH=aarch64le ./qnx/build-qnx.sh    # Raspberry Pi 5
TRANSPORT=posix ./qnx/build-qnx.sh       # socket transport, to compare
```

Under the hood this is `make QNX=1`, which compiles with `qcc -Vgcc_ntox86_64 -std=gnu11 -D_QNX_SOURCE`.

**If the build fails:** the native transport (`src/transport_qnx.c`) hadn't been compiled with `qcc` before this guide was written. The calls it uses are:

- `name_attach`, `name_detach`, `name_open`, `name_close`
- `MsgReceive`, `MsgReply`, `MsgError`, `MsgSend`
- `TimerTimeout`, `ConnectDetach`

Check each one against the QNX 8.0 C Library Reference. The most likely problems are a missing header or a type name (for example `rcvid_t`).

## 4. Deploy and run

```sh
./qnx/deploy.sh <target-ip> [user]
ssh <user>@<target-ip>
cd /tmp/bedside-guardian && sh scripts/on-target.sh
```

`on-target.sh` does four things:

1. prints `uname -a`
2. runs the 48 tests (`BG_SOURCE_DIR=. ./test_bg`)
3. runs the two-bed demo with the receive loop at `SCHED_FIFO` priority 30
4. while the demo runs, shows `/dev/name/local/` (where `bedside_guardian` is registered) and `pidin -p bgd`

## 5. What to capture for the video

- `uname -a` showing QNX 8.0 on the target
- `ls /dev/name/local/` showing `bedside_guardian`
- the demo console:
  - repeats folded
  - escalation to the charge nurse
  - the under-tier and stale acknowledgements refused
  - the charge nurse's acknowledgement with her words
  - the ventilator alarm reaching rapid response and the incident record
- `bgverify` reporting the chain intact, then catching a one-character edit
- the bench numbers from QNX (whatever they turn out to be), next to the same run with `TRANSPORT=posix`

## 6. Things worth trying once it runs

- **Run `bgsim` at different priorities** (`on -p 40 ./bgsim ...` versus `on -p 10 ...`) under load, and look at the guardian's latency numbers. That's QNX priority inheritance on message passing at work.
- **Register the event log as a resource manager** (`/dev/bedside-guardian/log`), so `cat` reads the verified chain head live.
- **Run the guardian in its own adaptive partition**, so a flood of LOW alarms can't starve escalation.
