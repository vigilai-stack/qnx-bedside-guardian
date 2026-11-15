# Devpost draft: QNX Bedside Guardian

**Status: draft only, not submitted.** Square brackets mark the things that can only be filled in after the guardian has run on QNX. Don't submit until they're filled in with real results.

- **Category:** Robotics & Medical
- **Tagline:** An alarm escalation service on QNX. Unanswered alarms move up on deadlines, and an acknowledgement only counts if it names a person and matches the alarm they saw.
- **Repository:** https://github.com/vigilai-stack/qnx-bedside-guardian (make it public before submitting)
- **Video:** [YouTube link, under 3 minutes, public]
- **Built with:** C11, QNX SDP 8.0, qcc, QNX native message passing, POSIX, GNU Make, GitHub Actions

---

## Inspiration

From my work in healthcare, one problem keeps coming back: the alarm nobody answers. A pulse oximeter on one bed fires while the nurse is two rooms away with a patient who fell. The alarm keeps sounding at a station nobody is standing at. Nothing says when it should become the charge nurse's problem. When someone finally presses acknowledge, the record says the alarm was acknowledged. It doesn't say who pressed it, what they saw or what they did.

Earlier I built Clinic Incident Guardian, a tracker for patient-safety and privacy incidents. Its core ideas are to keep the reporter's own words, to tie every approval to the exact version that was approved, and to chain the audit log so nobody can quietly change it. Those ideas belong on the unit floor too, where the deadline is seconds, not 60 days. On the floor, timing is the whole problem, and that's why I built this on QNX.

## What it does

Bedside Guardian is a QNX process that owns alarm deadlines for a unit.

- **Synthetic devices send alarms over QNX message passing.** Pulse oximeters, ventilators, infusion pumps and ECG monitors use `MsgSend` to reach a service registered as `bedside_guardian`. The device stays blocked until the guardian replies, so it knows the alarm was taken in.
- **Repeats fold.** A probe that keeps firing becomes one alarm with a count, not a stream of new ones. Repeats don't reset the clock.
- **Unanswered alarms escalate on deadlines.** A HIGH alarm goes from the bedside nurse to the charge nurse to rapid response, and rapid response is paged again until someone answers. A MEDIUM alarm stops at the charge nurse. A LOW alarm never escalates.
- **Acknowledgement is a gate, not a mute button.**
  - The person gives their name and role, plus the version and hash the console showed them.
  - If the alarm escalated or got worse in the meantime, the acknowledgement is refused as stale.
  - Once an alarm is the charge nurse's, a press from the bedside tier doesn't silence it.
  - A double tap counts once.
  - Their note is kept word for word.
- **Acknowledged doesn't mean gone.** If the device never says the condition cleared, the alarm comes back after a re-arm window.
- **Every step goes into a hash-chained log.** That includes every refusal. `bgverify` names the first line that was edited, removed or moved, and writes a one-page shift report.
- **Unanswered alarms become incidents.** When an alarm reaches rapid response unanswered, the guardian writes an incident record for the unit's safety review, with a plain-language summary. When someone finally acknowledges it, their note is added verbatim.

## How I built it

The project is plain C11 with no dependencies outside the C library, split into layers so the hard parts can be tested without a clock or an OS:

- **The engine** (`src/alarm.c`) is pure logic, and time is passed in as milliseconds. It uses a fixed alarm table, allocates no memory in the message path and does bounded work per message.
- **The event log** (`src/eventlog.c`) chains every line with SHA-256 (`src/sha256.c`, written from FIPS 180-4 and tested against the standard vectors). A restarted daemon verifies the log before appending, and refuses to extend a broken one.
- **The service** (`src/service.c`) turns wire messages into engine calls and replies, never trusts string termination from the wire, writes incident records and tracks latency from the sender's CLOCK_MONOTONIC timestamp.
- **There are two transports behind one interface.**
  - `src/transport_qnx.c` uses QNX native message passing:
    - `name_attach`, `MsgReceive` and `MsgReply` on the server side
    - `name_open` and `MsgSend` for clients
    - `_IO_CONNECT` and disconnect pulses handled
    - escalation deadlines kept by bounding each receive with `TimerTimeout` on CLOCK_MONOTONIC
  - `src/transport_posix.c` uses a Unix socket, so I could develop and test on Linux, and it can also be built on QNX for comparison.
- **The programs:**
  - `bgd`, the guardian, with optional `SCHED_FIFO` and `mlockall`
  - `bgsim`, which replays timed scenarios of devices and staff and also benchmarks
  - `bgack`, the console
  - `bgverify`, which checks the chain and writes the report
- **Testing:**
  - 48 unit tests
  - five scripted shifts whose expected event logs are pinned by a SHA-256 manifest
  - an end-to-end run with a real daemon and real IPC, which also tampers with the log and restarts the daemon
  - CI on gcc and clang with `-Werror`, plus an AddressSanitizer and UndefinedBehaviorSanitizer run
- **The QNX build** is `make QNX=1` (qcc -Vgcc_ntox86_64), wrapped in `qnx/build-qnx.sh`, with `qnx/deploy.sh` and `qnx/on-target.sh` for the target.

[After the QNX run: say which target was used (QEMU x86_64, VMware or Raspberry Pi 5), which SDP 8.0 install, and paste the real bench numbers from QNX next to the POSIX-transport numbers.]

## How it uses QNX (for reviewers)

| What to check | Where |
|---|---|
| Service registration | `src/transport_qnx.c`, `bg_server_open()` (`name_attach`) |
| Receive, pulses, connect handling, deadline timeout | `src/transport_qnx.c`, `bg_server_receive()` (`MsgReceive`, `TimerTimeout`, `ConnectDetach`) |
| Reply and client send | `bg_server_reply()` (`MsgReply`), `bg_client_call()` (`MsgSend`) |
| Message layout kept below `_IO_BASE` | `include/bg/proto.h` |
| Real-time options | `apps/bgd.c` (`--rt` SCHED_FIFO, `--lock` mlockall) |
| qcc build and target scripts | `Makefile` (`QNX=1`), `qnx/` |
| Running on QNX 8.0 | [timestamps in the video] |

## Challenges I ran into

- **Deciding what "acknowledged" should mean.** A plain button press is easy and nearly useless. I settled on making a person acknowledge a specific version of the alarm, the one on their screen. Anything that changes what they would have seen (an escalation, a priority upgrade) makes the old acknowledgement stale. Small value changes don't, because a reading moving from 85 to 84 every second would make the alarm impossible to acknowledge.
- **Alarm fatigue versus missed alarms.** Folding repeats cuts noise, but folding the wrong things could hide a real change. Repeats fold only when bed, device and condition all match. A worse priority starts a new cycle, and repeats never reset the escalation clock.
- **Who is allowed to silence an escalated alarm.** I chose to refuse a press from below the current tier, so an escalation can't be quietly undone. It's a policy choice, and a real unit might choose differently, so the README says so.
- **Building for an OS I couldn't run in CI.** The QNX SDP needs a license download, so CI can't build it. I kept every QNX call in one file behind an interface, tested everything else on Linux, and added a stub type-check so typos in the QNX file get caught early. [After the QNX run: what broke on the first qcc build and how I fixed it.]
- **Making the log mean something.** A hash chain only proves order and integrity from its head. So the incident records carry the log head at the moment they're written, and a restarted daemon refuses to append to a log that doesn't verify.

## Accomplishments that I'm proud of

- **The acknowledgement gate catches real mistakes.** It refuses an acknowledgement from an old screen and a press from the wrong tier, and it counts a double tap once. Every refusal lands in the log with its reason.
- **The unanswered-ventilator case runs start to finish.** It escalates through all three tiers, files an incident, and records the rapid response nurse's own explanation verbatim.
- **The scenarios are locked.** Any change in behavior shows up as a failing test.

## What I learned

- QNX message passing is a natural fit for alarms. A synchronous send means the device knows its alarm was taken in, and the receiving thread runs at the sender's priority.
- Most of the safety in a system like this is in small rules: what counts as the same alarm, what makes an acknowledgement stale, when an acknowledged alarm comes back. Writing those rules down as tests made them much easier to get right.

## What's next

- Run the guardian in its own adaptive partition, so a flood of low-priority traffic can't starve escalation.
- Expose the verified log head through a resource manager, so any tool can read it.
- A real nurse-station display instead of the command-line console.
- A device gateway for a real standard such as IEEE 11073 SDC, instead of scripted devices.

## Note on what's new during the hackathon

The repository was created on November 15, 2026, during the submission period. The ideas about version-bound approval, verbatim notes and a chained log come from my earlier Clinic Incident Guardian project, but no code was reused. That project is a Python web app, and this one is new C code written for QNX.

---

*Before submitting:*

- [ ] Run it on QNX and fill in the brackets.
- [ ] Record the video and set it to public.
- [ ] Make the repo public and check that the MIT license shows in the About box.
- [ ] Remove this checklist from the pasted text.
