# Planets Plan

Tracks the Mercury-Uranus real-ephemeris planets feature. Read this at the start of any
planets-related session; full architecture writeup lives in `CLAUDE.md`'s "Subsystem: Planets" —
this file is the session log and the forward-looking punch list, not a duplicate of that design doc.

---

## Status: shipped (session 30, 2026-07-30)

Mercury, Venus, Mars, Jupiter, Saturn, Uranus render as clickable points of light with real
Keplerian-approximation orbital positions (JPL/Standish elements, valid 1800-2050 — comfortably
covers the sim's fixed 2036-06-21 epoch), true per-planet apparent magnitude (Paul Schlyter's
standard formulas), and per-planet approximate true color. The Moon's direction calculation was
also fixed in the same session (was a circular-equatorial orbit with a phase constant hand-
calibrated for a stale epoch; now uses the same Keplerian approach, geocentric instead of
heliocentric).

**Architecture, one paragraph:** not built on the satellite orbital-compute pipeline
(`GpuSatOrbit`/`sat_orbit.comp`) — that solves Earth-relative near-field geometry (shadow,
attitude, 7-day orbit rebake) planets don't have. Instead: CPU-side closed-form ephemeris math in
`updatePositions()` (same pattern as the existing Sun/Moon code), rendered by reusing the star
point-sprite pipeline unchanged (a planet is architecturally "a direction + phase-driven
brightness + size," exactly like a star, just recomputed every frame instead of once at init).
Picking (`pickPlanetAt()`) is cheaper than satellite picking since the planet buffer is already
host-mapped — no GPU staging copy needed at all.

**A real bug was caught and fixed during implementation:** the first phase-angle formula had a
sign error that would have shown every planet's illuminated fraction backwards (a near-full
Jupiter would have displayed as ~0% lit). Caught by manually re-deriving the vector algebra and
verifying numerically — see `CLAUDE.md`'s subsystem doc and the memory entry
`feedback_verify_geometry_numerically` for the lesson.

**Session 30 follow-up (same day):** added per-planet approximate true color
(`kPlanetColor[kPlanetCount]`, next to `kPlanetElements` in `SatelliteSim.cpp`) — all six planets
previously shared a single hardcoded near-white `baseColor`. Mars is the one that actually reads
as visibly colored at this scale (rust/salmon); the gas giants and Mercury stay close to their old
near-white since real cloud-top/regolith colors are genuinely subtle, Venus/Uranus get a faint
warm/cool cast. No shader changes needed — `star_point.frag`'s existing intensity-driven
desaturation curve (bright = full tint, faint = fades to white) already applies correctly to
planets for free, since it was written generically against `fragColor`/`fragIntensity`, not
star-specific.

### Known simplifications (accepted, not bugs)

- **Saturn's ring brightness is omitted.** Its magnitude formula uses only the disk's phase-angle
  term; real Saturn is measurably brighter (~0.5-1 mag) when the rings are open toward Earth. Would
  need Saturnicentric ring-plane geometry (ring-plane inclination angle as seen from Earth) to add
  properly.
- **Neptune is excluded by design**, not an oversight — it's never naked-eye (~mag 7.8, below even
  the star catalog's mag-6.5 floor). Trivial to add later if wanted (same `KeplerElements` table
  has its row, just needs a magnitude-formula case and `kPlanetCount` bumped).
- **No dwarf planets/asteroids** (Ceres, Vesta, Pallas) — same reasoning as Neptune, all fainter
  than naked-eye except Vesta at rare oppositions; not attempted.
- **Moon fix is direction-only.** The illuminated-fraction formula and all of `sat_sky.frag`'s
  disc-rendering/phase-shading/earthshine code are untouched — only the upstream direction
  derivation improved.

---

## Next steps (not started, no priority order implied)

1. **In-app visual QA** — this is the main open item. Nothing here has been seen running; the
   project's standing rule is that Claude doesn't launch the app itself. Worth checking:
   positions/motion look plausible over a session (do they visibly move against the star field at
   high time-warp?), Mars's color reads as intended, click-select info panel formatting, and
   whether `lightPollutionGain` (already flagged as needing a look post-S2c) also needs a nudge now
   that Venus/Jupiter are real bright additions to the night sky.
2. **Attributions.** `RELEASE_v1_1_PLAN.md`'s UC2 (blocked on the user's asset-provenance answers)
   should eventually cite the JPL/Standish elements table and Paul Schlyter's magnitude formulas —
   same spirit as the existing Yale Bright Star Catalogue citation, since both are "public formula/
   dataset used verbatim" rather than original code.
3. **Saturn's rings**, if wanted: both the brightness contribution above and, as a bigger stretch,
   an actual visible ring — the latter would need something closer to the Moon's disc-rendering
   approach (a textured/procedural shape in `sat_sky.frag`) rather than the point-sprite pipeline,
   since a point sprite can't show ring geometry. Meaningfully bigger scope than anything else on
   this list.
4. **Neptune / dwarf planets**, if wanted for completeness rather than naked-eye realism — cheap
   (table entries + a magnitude case), explicitly deferred rather than rejected.
5. **Gamepad/controller selection.** `pickPlanetAt()` is wired into the same `KB_SELECT_SAT`
   center-screen path satellites use, so controller selection already works today. No known gap —
   listed here only so a future UC4 (dual-input) session doesn't have to rediscover that.
