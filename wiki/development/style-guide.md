# Wiki style guide

This wiki describes SAT LIGHT SIM **as it is now**. It is reference literature, not a log. Every page outside
[History](../history/index.md) must read as if the system had always been the way it is today.

## The one rule: present tense, present system

A main page says what the code does and why that design is right. It never says what the code used to do.

| Don't write | Write |
|---|---|
| "Review 22 made rain fall at 10 m/s (it was 8)." | "Rain falls at about 10 m/s." |
| "Until 2026-09-22 the buffers were sized to 10M satellites." | "The per-satellite buffers are sized to the loaded roster (about 100 B per satellite)." |
| "The first cut used a Gaussian, which lit 16× the area; it is now a disk." | "A beam is a disk: the Sun's limb-darkened image in the mirror. A Gaussian of the same radius would light about 16 times the area." |
| "Fixed a bug where clouds vanished at 6 km." | *(nothing on the main page; the bug belongs in History if anywhere)* |

**Rationale is not history.** "Why it is this way" belongs on the main page when it is phrased as a property
of the design: *"The depth buffer is R32F because a half float tops out at 65 504, which a near-horizon cloud
distance exceeds."* That stays. *"This was RGBA16F until a bug in September"* goes to History.

**Rejected alternatives** may appear on a main page when they explain the current design and are phrased
timelessly ("Sharing `optDepth` between the two shaders defeats loop unrolling and measures slower"). The
story of how that was discovered goes to [Design notes](../history/design-notes.md).

Banned on main pages (the checker flags them): dates, review/pass/session numbers, "used to", "no longer",
"was replaced", "first cut", "tried and reverted", "until <date>", "fixed". If a sentence genuinely needs one
(for example a real-world date such as an eclipse), end the line with `<!-- history-ok -->`.

## Audience

Write for a capable engineer or modder who has not read the code. Define a term the first time a page uses
it. Prefer one clear sentence over a dense parenthetical chain. Section names in `CLAUDE.md` are dense
working notes; a wiki page is the explained version.

Each section has its own reader:

| Section | Reader | Depth |
|---|---|---|
| Using | a player | what it does, how to use it; no internals |
| Modding | someone editing JSON | every field, worked examples, what goes wrong |
| Simulation, Accuracy | a scientist checking the physics | models, equations, assumptions, measured error |
| Rendering | a graphics programmer | techniques, data flow, costs, invariants |
| Sound | either | behaviour, then the table format |
| Development | a contributor | how to build, change and test safely |
| History | the curious | dated, may mention bugs and reversals |

## Page shape

1. `# Title`, then one or two sentences saying what the page covers and what it doesn't.
2. Sections in the order a reader needs them: concept, then mechanism, then parameters, then pitfalls.
3. **Invariants** (things that silently break if violated) go in a `!!! warning "Invariant"` admonition.
4. A short **Where in the code** section at the end listing the files and main functions.
5. **Settings** that tune the behaviour: a table with the UI label, the `settings.json` key and the default.

Aim for 150 to 600 lines a page. Split a page that grows past that.

## Formatting

- **Code references** are code spans with repo-relative paths: `src/simulations/SatelliteSim.cpp`,
  `shaders/cloud_v2_march.comp`. Name functions as `recordCompute()`. No line numbers (they rot).
- **Math** uses MathJax: inline `\( m = -2.5 \log_{10} F \)`, display `\[ ... \]`.
- **Diagrams** use Mermaid fences (`` ```mermaid ``) for pass graphs and data flow; ASCII art is fine for
  small layouts.
- **Units** always: m, km, ms, deg, mag. Write numbers as they appear in the code's defaults.
- **Links** between wiki pages are relative (`../rendering/terrain.md#the-height-function`). Link the first
  mention of a concept that has its own page.
- **Admonitions**: `!!! note`, `!!! tip`, `!!! warning "Invariant"`, `??? info "Details"` (collapsed) for
  long derivations.
- No emoji, no marketing tone. British or American spelling, but consistent within a page.

## Accuracy over coverage

Every claim on a page must be true of the current code. When a source note (`CLAUDE.md`, a plan) disagrees
with the code, **the code wins**. Write down the disagreement in the page's inbox note or in
[Known issues](../history/known-issues.md), not on the page. When you can't verify something, leave it out
or mark it `!!! question "Unverified"`. Don't guess.
