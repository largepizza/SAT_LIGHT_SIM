# Changelog

## v1.1.1 — 2026-09-08

### Added
- **Zodiacal light** — sunlight scattered by interplanetary dust rendered as a faint warm
  cone along the ecliptic, with a dim gegenschein patch opposite the sun. New Settings ›
  Clouds sliders: *Zodiacal gain*, *Zodiacal width*, *Zodiacal outer fade*.
- **Dark-sky exposure model** (`darksky.glsl`) — per-direction, per-sample sky-brightness
  gate shared by the Milky Way, zodiacal light and their ocean reflections, so skyglow and
  twilight thin faint features instead of just dimming them uniformly. New Settings ›
  Photometry sliders: *City sky mag*, *Twilight sky mag*, *Twilight end*, *Twilight aniso*.
- **Milky Way ocean reflection** with its own *Ocean MW refl* gain slider.
- **Minimal constellation preset** (`data/custom/constellations_minimal.json`, ~124k
  satellites) alongside the full default set.
- **Invert look axes** — Settings › Controls toggles for Mouse X/Y and Controller X/Y,
  persisted in `settings.json`.

### Changed
- Default constellation set expanded to ~1.38M satellites (larger Starlink Gen2, Guowang,
  SpaceX AI and debris populations).
- Vertical (raise/lower elevation) speed no longer approaches the terrain exponentially — it
  now keeps a brisk fixed floor speed near the surface, so pushing into the ground and
  climbing back out take a normal amount of time instead of feeling stuck.
- Holding sprint (Move Fast) now cancels fine/slow movement mode instead of being overridden
  by it.
- New Milky Way skybox: ESO's "The Milky Way panorama" (eso0932a, credit ESO/S. Brunier,
  CC BY 4.0), replacing the previous Solar System Scope star texture.
- Fine-movement key (stick-click / Ctrl) is now a latching toggle instead of a held modifier.
- Milky Way sun-glare suppression only applies while the sun is above the horizon, so the
  band no longer dims toward an Earth-occluded sun.
- Controls reference window is now opened from a button in Settings › Controls (replacing
  the "show on startup" toggle); intro captions restyled.
- App now anchors its working directory to the executable location, fixing asset loading
  when launched from Finder or another directory.
- Cloud *Ambient* gain default raised to 1.0.
- *City sky mag* (Photometry) default lowered to 13.0 and its slider now goes down to 1.0,
  below the real-world range, for stronger artistic washout of dark-sky features under city glow.

### Build / packaging
- New `SatLightSimFresh` build target (exe `SatLightSim_FRESH`) — same binary compiled with
  `SAT_FRESH_SETTINGS` so it never reads or writes `settings.json`, for testing the out-of-box
  first-run experience. Ships with its own VS Code build task and debug launch configs.
- Cross-platform CMake presets (`CMakePresets.json`) and a single `package-release` target
  (`cmake/PackageRelease.cmake`) driving CI and `release.bat`.
- macOS release is now a universal (arm64 + x86_64) binary with a fixed deployment target.
- Machine-specific absolute paths scrubbed from committed config; `CMAKE_POLICY_VERSION_MINIMUM`
  set so the project configures under CMake 4.
- Device limits and required features are logged and validated at startup.
- CI Windows job builds with Ninja + MSVC (`windows-ci` preset) instead of a hardcoded
  Visual Studio generator version, so a GitHub runner-image toolchain bump no longer breaks
  the release build. Release workflow also runs on pushes/PRs to `main`, not only on tags.
- Fixed a `float`→`int` narrowing conversion in a settings-slider table that MSVC accepted
  but GCC/Clang reject, which broke the Linux and macOS release builds.

### Fixed
- Satellite/star terrain occlusion at render scale < 100% (low-res sky prepass writes no
  depth) now matches the full-res path, including correct behaviour in orbital views.
