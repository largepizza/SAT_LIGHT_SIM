# Mission UX: milestones, the Info window's MISSION section, window opacity (2026-10-20)

Pages: using (Info window, Missions window, keys, bookmarks), launches (simulation), development (harness).

- **Mission milestones.** Every launch object has a timeline built from its launch's phase times: lift-off, max-Q,
  MECO and staging, fairing separation, SECO, deployment begins / last satellite released, the upper stage's deorbit
  burn and entry (burn-up, or a returning ship's entry interface, belly-flop descent, flip and landing burn, tower catch
  or splashdown), docking for crew flights. The booster's own milestones (boostback, entry burn, landing burn, catch /
  landing / droneship) are read from its flight track. Trains: release, start of raising, arrival in the slots;
  retirees: start of deorbit, re-entry. A satellite in operations shows the launch that put it there, its time in its
  slot, design life used and when its replacement arrives.
- **Skipping.** Next / previous milestone jumps the clock to 10 s before it (less where milestones crowd), at 1x,
  keeping the selection and Go to on the same object (the rocket, its booster, a train member, a retiree) at the new
  time. Controls: keys `]` / `[` ("Next / Previous Mission Milestone", rebindable), the Go to bar's `<` and
  `Next: ... >`, the Info window's MISSION section (Previous / Next and a Skip / Back button per milestone), and
  "Next >" on the Missions window's Ongoing launch rows.
- **Info window.** New MISSION section (after STATUS, open): mission, object, facts, progress, the milestones with T+
  from lift-off (or "in" / "ago") and UTC, the current one highlighted. The actions are one row of four equal buttons:
  Trace pass, Go to, Track, Explode (danger colour).
- **Missions window.** The Sun's Fury card is pinned under the Upcoming list (always visible).
- **Window opacity** (Display tab, `display.window_opacity`, 60-100 %, default 100 % = opaque). A window over another
  now hides it completely, including the other window's view chips and labels.
- **Selection of re-entries.** A selected or followed upper stage stays selected through its re-entry even when the
  fireball cannot be seen from the observer (behind the Earth, far away, daylight).
- **New default bookmark** "Starmind Deployment": a sunlit Starship deployment of Starmind AI1 satellites (paused).
- Harness: `missions milestone [list|next|prev|<i>] [row=]`, `ui key ]` / `[`; `select const` / `select sat` on a
  destroyed satellite is an error; `ui close all` also closes the Issue reports window.
