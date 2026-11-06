# Look baselines

Full copies of the user's `settings.json` taken when the user called the look a good baseline. They are the
fallback if later tuning ruins the clouds, lighting or photometry.

- `look_2026-11-05.json`: cloud shape, lighting and photometry after the 2026-11-05 cloud pushes (perf-frames 076d683).

To compare against one, run the harness with `--settings tools/harness/baselines/<file>`. To restore one, copy its
`clouds`, `clouds_v2` and `photometry` sections back into `user_data/settings.json` with the app closed.
