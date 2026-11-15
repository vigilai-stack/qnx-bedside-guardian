# Demo script (about 3 minutes)

All data is synthetic. On the video, run it on the QNX target (see `docs/QNX.md`). On Linux, `make demo` shows the same thing.

## 0:00 to 0:20 — the problem

"On a hospital unit, an alarm that nobody answers is a timing failure. The nurse is in another room, the alarm keeps sounding, and there's no rule for when it becomes someone else's job. When someone finally presses acknowledge, nobody knows what they saw. This is a guardian process on QNX that owns those deadlines."

Show `uname -a` (QNX 8.0) and `ls /dev/name/local/` with `bedside_guardian` registered.

## 0:20 to 1:30 — bed 12, low oxygen

Run `sh scripts/on-target.sh`, or `BUILD=. sh scripts/demo.sh 50`.

- The pulse oximeter fires three times. One alarm, count x3. "Repeats don't make new noise, and they don't reset the clock."
- Fifteen seconds (seven and a half at demo speed) and it **escalates** to the charge nurse.
- The bedside nurse presses acknowledge: **ERR_ROLE**. "It's the charge nurse's alarm now. The press from below doesn't silence it."
- The charge nurse acknowledges from an old screen: **ERR_STALE**. "She has to acknowledge the version she's looking at, not one from before the escalation."
- She acknowledges the current one. Her words are kept exactly: "Probe had slipped off a cold finger, moved it to the earlobe, reading 95."
- Her double tap: **OK_DUPLICATE**, counted once.
- The device clears.

## 1:30 to 2:15 — bed 4, the ventilator nobody answers

- Bedside, then charge nurse, then **rapid response**, with an **INCIDENT** line.
- Show `demo-out/incidents.jsonl`: the incident record, then rapid response's acknowledgement with the note verbatim: "Bedside nurse was in bed 6 with a fall."
- "That record goes straight to the unit's safety review. Nobody has to rebuild the timeline from memory."

## 2:15 to 2:45 — the record holds up

- `./bgverify demo-out/events.log --html demo-out/shift-report.html`: intact.
- Edit one character in the log (`sed 's/Okafor/0kafor/'`), verify again: **BROKEN at line N, changed after it was written**.
- Show the shift report: time to acknowledge, who acknowledged, their words, refused acknowledgements.

## 2:45 to 3:00 — why QNX

- Show the bench line.
- "Every device message is a synchronous MsgSend, so the device knows the guardian has it. The kernel serves it at the sender's priority, and the escalation clock is a kernel timeout on the receive. Synthetic data only. This isn't a medical device."
