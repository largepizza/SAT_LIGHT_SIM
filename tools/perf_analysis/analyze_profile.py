#!/usr/bin/env python3
"""Analyze SatelliteSim perf snapshots from perf_profiles/profile_log.jsonl.

Each line of the log is one JSON record written by the in-app "Save Snapshot"
button (Settings > Display). This script loads the whole log, flattens the
nested fields, and reports:
  - mean GPU pass duration per resolution bucket, plus per-megapixel cost
    (a flat per-megapixel cost across resolutions confirms a pass is purely
    resolution-bound; a rising one means something else scales with it too)
  - Pearson correlation of each major GPU pass against scene/observer
    variables (altitude, camera elevation, pixel count)
  - two PNG scatter/line plots under analysis_output/

Usage:
    .venv/Scripts/python.exe analyze_profile.py [path/to/profile_log.jsonl]

With no argument, searches build*/**/perf_profiles/profile_log.jsonl under
the repo root.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

GPU_COL_RENAME = {
    "gpu_timing_ms.cloud_march": "cloud_march",
    "gpu_timing_ms.orbit_compute": "orbit_compute",
    "gpu_timing_ms.flare_compute": "flare_compute",
    # Added alongside cloud_shadow.comp (session 32). Snapshots older than that have no
    # such key; without this mapping the bucket was silently dropped from every report.
    "gpu_timing_ms.cloud_shadow_map": "cloud_shadow_map",
    # Added in the pipeline-unification pass. Before it existed, beam_cloud_block.comp's
    # cost was folded into orbit_compute - so orbit_compute is NOT comparable across the
    # boundary where this key appears. Treat a jump down in orbit_compute at that point as
    # the split landing, not as a real optimisation.
    "gpu_timing_ms.beam_cloud_block": "beam_cloud_block",
    # Pre-split legacy bucket (recordDraw's sky bg + satellite points + stars fused into
    # one timestamp) - present only in snapshots captured before the sky/satellite draw
    # split. Kept as its own column rather than merged into sky_background_draw so the
    # two are never silently averaged together.
    "gpu_timing_ms.sky_terrain_draw": "sky_terrain_draw_legacy",
    "gpu_timing_ms.sky_background_draw": "sky_background_draw",
    "gpu_timing_ms.satellite_star_draw": "satellite_star_draw",
    "gpu_timing_ms.ui_overlay": "ui_overlay",
    "gpu_timing_ms.total": "gpu_total",
}


def find_default_log(repo_root: Path) -> Path | None:
    candidates = sorted(repo_root.glob("build*/**/perf_profiles/profile_log.jsonl"))
    return candidates[-1] if candidates else None


def load(path: Path) -> pd.DataFrame:
    raw = pd.read_json(path, lines=True)
    return pd.json_normalize(raw.to_dict(orient="records"))


def add_derived_columns(df: pd.DataFrame) -> pd.DataFrame:
    df = df.rename(columns=GPU_COL_RENAME)
    for col in ("sky_background_draw", "satellite_star_draw", "sky_terrain_draw_legacy",
                "cloud_shadow_map", "beam_cloud_block", "debug_disable_mask"):
        if col not in df.columns:
            df[col] = float("nan")

    # sky_terrain_draw: best-available "sky/terrain/ocean/cloud-composite shader" cost -
    # the new isolated sky_background_draw where present, else the pre-split legacy bucket
    # (which also included satellite+star draw) for older snapshots. Lets resolution/altitude
    # analyses below span both schemas; knockout-toggle analysis should use
    # sky_background_draw directly since it needs the split, not this coalesced view.
    df["sky_terrain_draw"] = df["sky_background_draw"].where(
        df["sky_background_draw"].notna(), df["sky_terrain_draw_legacy"]
    )

    df["pixel_count"] = df["resolution.width"] * df["resolution.height"]
    df["megapixels"] = df["pixel_count"] / 1_000_000

    for col in ["cloud_march", "sky_terrain_draw", "gpu_total"]:
        df[f"{col}_per_mpx"] = df[col] / df["megapixels"]

    df["cloud_frac"] = df["cloud_march"] / df["gpu_total"]
    df["terrain_frac"] = df["sky_terrain_draw"] / df["gpu_total"]
    return df


def print_resolution_breakdown(df: pd.DataFrame) -> None:
    print("\n=== GPU pass cost by resolution (ms) ===")
    grp = df.groupby(["resolution.width", "resolution.height"])
    cols = ["cloud_march", "sky_terrain_draw", "cloud_shadow_map", "beam_cloud_block",
            "orbit_compute", "flare_compute", "ui_overlay", "gpu_total"]
    cols = [c for c in cols if df[c].notna().any()]
    print(grp[cols].agg(["mean", "std", "count"]).round(2).to_string())

    print("\n=== Per-megapixel cost (ms/Mpx) - flat across rows = purely resolution-bound ===")
    per_mpx_cols = ["cloud_march_per_mpx", "sky_terrain_draw_per_mpx", "gpu_total_per_mpx"]
    print(grp[per_mpx_cols].agg(["mean", "std"]).round(3).to_string())


def print_matched_altitude_ratios(df: pd.DataFrame) -> None:
    # Correlation across the whole dataset conflates "cost changed because resolution
    # changed" with "cost changed because altitude/scene also changed between samples".
    # This isolates the resolution effect by only comparing samples captured at (nearly)
    # the same altitude across different resolutions - a same-conditions paired swap.
    widths = df["resolution.width"].unique()
    if len(widths) < 2:
        return
    lo_w, hi_w = sorted(widths)[0], sorted(widths)[-1]
    lo_px = df.loc[df["resolution.width"] == lo_w, "pixel_count"].iloc[0]
    hi_px = df.loc[df["resolution.width"] == hi_w, "pixel_count"].iloc[0]
    pixel_ratio = hi_px / lo_px

    df = df.copy()
    df["alt_bucket"] = df["observer.height_offset_m"].round(-1)  # nearest 10 m
    lo = df[df["resolution.width"] == lo_w].groupby("alt_bucket")[["cloud_march", "sky_terrain_draw", "gpu_total"]].mean()
    hi = df[df["resolution.width"] == hi_w].groupby("alt_bucket")[["cloud_march", "sky_terrain_draw", "gpu_total"]].mean()
    shared = lo.index.intersection(hi.index)
    if len(shared) == 0:
        print("\n=== Matched-altitude resolution ratio ===\nNo two resolutions share a near-identical altitude - can't isolate resolution from altitude yet.")
        return

    print(f"\n=== Matched-altitude resolution ratio ({lo_w}x -> {hi_w}x, pixel count ratio = {pixel_ratio:.2f}x) ===")
    print("Ratio < pixel ratio means sub-linear scaling (fixed per-frame overhead diluting the per-pixel win).")
    ratio = (hi.loc[shared] / lo.loc[shared]).round(2)
    ratio.index.name = "altitude_m"
    print(ratio.to_string())
    print("\nMean ratio across matched altitudes:")
    print(ratio.mean().round(2).to_string())


KNOCKOUT_BIT_NAMES = {
    1: "terrain march",
    2: "atmosphere loop (N_VIEW)",
    4: "sun optical depth (N_LIGHT)",
    8: "ocean sky reflection",
    16: "airglow red (16-step march)",
    32: "aurora curtain march",
    64: "cloud self-shadow cone",
    128: "Reflect-Orbital beams",
    256: "cloud shadow map (dispatch)",
    512: "beam cloud block (dispatch)",
}

# Which GPU bucket each knockout actually affects. The consumer-side bits all live inside
# the sky fragment shader; the two producer-side bits skip a whole compute dispatch, so their
# cost shows up in that dispatch's own bucket and NOT in sky_background_draw at all. Reporting
# every bit against sky_background_draw (as this script used to) makes the producer bits look
# free even when they aren't.
KNOCKOUT_BIT_BUCKET = {
    64: "cloud_march",
    128: "cloud_march",   # the volumetric tube term dominates; the ground-spot term is in sky bg
    256: "cloud_shadow_map",
    512: "beam_cloud_block",
}

# A snapshot pair is only comparable if it was captured at effectively the same viewpoint.
# These are the tolerances for "same site" clustering; the time gap dominates in practice
# because toggles are swept in quick succession at one spot.
CLUSTER_MAX_GAP_S = 900.0     # 15 min without a snapshot starts a new cluster
CLUSTER_LATLON_TOL = 0.5      # degrees
CLUSTER_ALT_REL_TOL = 0.25    # 25% change in observer altitude
CLUSTER_EL_TOL = 15.0         # degrees of camera elevation


def assign_clusters(df: pd.DataFrame) -> pd.DataFrame:
    """Group snapshots into comparable 'same site, same sitting' clusters.

    Without this, comparing a toggle's global mean against the global mask=0 mean conflates
    "this toggle was disabled" with "these snapshots were taken somewhere else entirely".
    On the existing dataset that produced *negative* isolated costs for every toggle - the
    baseline happened to be captured at a much cheaper viewpoint than the toggled samples.
    """
    df = df.sort_values("captured_at_unix").copy()
    lat = df["observer.lat_deg"]
    lon = df["observer.lon_deg"]
    alt = df["observer.height_offset_m"]
    el = df["camera.el_deg"]

    gap = df["captured_at_unix"].diff() > CLUSTER_MAX_GAP_S
    moved = (lat.diff().abs() > CLUSTER_LATLON_TOL) | (lon.diff().abs() > CLUSTER_LATLON_TOL)
    climbed = (alt.diff().abs() / (alt.shift().abs() + 1000.0)) > CLUSTER_ALT_REL_TOL
    turned = el.diff().abs() > CLUSTER_EL_TOL

    df["cluster"] = (gap | moved | climbed | turned).fillna(True).cumsum()
    return df


def print_knockout_summary(df: pd.DataFrame) -> None:
    # Reads debug_disable_mask snapshots captured via the Display tab's knockout checkboxes.
    # Compares each single-toggle-disabled sample against the mask=0 baseline FROM THE SAME
    # CLUSTER - the in-app alternative to a GPU capture tool. See assign_clusters() for why
    # a global mean is not good enough.
    known = df[df["debug_disable_mask"].notna()].copy()
    if known.empty:
        print("\n=== Knockout toggle summary ===\nNo snapshots with knockout toggles captured yet.")
        return
    known["debug_disable_mask"] = known["debug_disable_mask"].astype(int)
    known = assign_clusters(known)

    rows = []
    skipped_clusters = 0
    for cid, cl in known.groupby("cluster"):
        base = cl[cl["debug_disable_mask"] == 0]
        if base.empty:
            skipped_clusters += 1
            continue
        for bit, name in KNOCKOUT_BIT_NAMES.items():
            sub = cl[cl["debug_disable_mask"] == bit]
            if sub.empty:
                continue
            bucket = KNOCKOUT_BIT_BUCKET.get(bit, "sky_background_draw")
            if bucket not in cl.columns or base[bucket].isna().all():
                continue
            base_ms = base[bucket].mean()
            off_ms = sub[bucket].mean()
            rows.append({
                "toggle": name,
                "bucket": bucket,
                "cluster": int(cid),
                "alt_m": int(base["observer.height_offset_m"].mean()),
                "baseline_ms": round(base_ms, 2),
                "with_toggle_ms": round(off_ms, 2),
                "isolated_cost_ms": round(base_ms - off_ms, 2),
                "pct_of_bucket": round(100 * (base_ms - off_ms) / base_ms, 1) if base_ms else float("nan"),
                "fps_delta": round(sub["cpu_frame.fps"].mean() - base["cpu_frame.fps"].mean(), 1),
            })

    print("\n=== Knockout toggle summary (per matched cluster: same site, same sitting) ===")
    if not rows:
        print("No cluster contains both a mask=0 baseline and a single-toggle snapshot.")
        print("Capture the baseline AND each individual toggle without moving the camera between them.")
        return
    out = pd.DataFrame(rows).sort_values(["toggle", "cluster"])
    print(out.set_index(["toggle", "cluster"]).to_string())

    print("\n--- Mean isolated cost across clusters (only clusters with a matched baseline) ---")
    agg = out.groupby(["toggle", "bucket"]).agg(
        clusters=("cluster", "nunique"),
        isolated_cost_ms=("isolated_cost_ms", "mean"),
        pct_of_bucket=("pct_of_bucket", "mean"),
        fps_delta=("fps_delta", "mean"),
    ).round(2)
    print(agg.to_string())
    if skipped_clusters:
        print(f"\n({skipped_clusters} cluster(s) skipped - toggled snapshots with no mask=0 baseline captured alongside.)")


def print_correlations(df: pd.DataFrame) -> None:
    n = len(df)
    print(f"\n=== Correlation of GPU pass duration with scene/observer variables (Pearson r, n={n}) ===")
    print("Small sample - treat as directional, not statistically rigorous.")
    targets = ["cloud_march", "sky_terrain_draw", "gpu_total"]
    predictors = {
        "pixel_count": df["pixel_count"],
        "observer.height_offset_m": df["observer.height_offset_m"],
        "camera.el_deg": df["camera.el_deg"],
        "observer.terrain_h_m": df["observer.terrain_h_m"],
    }
    rows = []
    for t in targets:
        row = {"pass": t}
        for name, series in predictors.items():
            row[name] = df[t].corr(series)
        rows.append(row)
    print(pd.DataFrame(rows).set_index("pass").round(2).to_string())


def make_plots(df: pd.DataFrame, out_dir: Path) -> None:
    out_dir.mkdir(exist_ok=True)
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))

    grp = df.groupby("megapixels")
    for col, label, color in [
        ("cloud_march_per_mpx", "Cloud march", "tab:blue"),
        ("sky_terrain_draw_per_mpx", "Sky+terrain draw", "tab:orange"),
    ]:
        means = grp[col].mean()
        axes[0].plot(means.index, means.values, "o-", label=label, color=color)
    axes[0].set_xlabel("Resolution (megapixels)")
    axes[0].set_ylabel("ms / megapixel")
    axes[0].set_title("Per-pixel cost by resolution")
    axes[0].legend()
    axes[0].grid(alpha=0.3)

    axes[1].scatter(df["observer.height_offset_m"] + 1, df["cloud_march"], label="Cloud march", color="tab:blue")
    axes[1].scatter(df["observer.height_offset_m"] + 1, df["sky_terrain_draw"], label="Sky+terrain draw", color="tab:orange")
    axes[1].set_xscale("log")
    axes[1].set_xlabel("Observer altitude AGL (m, log scale)")
    axes[1].set_ylabel("ms")
    axes[1].set_title("Pass cost vs. observer altitude")
    axes[1].legend()
    axes[1].grid(alpha=0.3, which="both")

    fig.tight_layout()
    out_path = out_dir / "perf_scatter.png"
    fig.savefig(out_path, dpi=140)
    print(f"\nSaved plot: {out_path}")


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent.parent

    if len(sys.argv) > 1:
        log_path = Path(sys.argv[1])
    else:
        found = find_default_log(repo_root)
        if found is None:
            print("No profile_log.jsonl found under build*/. Pass a path explicitly.")
            sys.exit(1)
        log_path = found

    print(f"Loading {log_path}")
    df = load(log_path)
    df = add_derived_columns(df)

    print(f"\n{len(df)} snapshot(s) loaded.")
    if "gpu_device" in df.columns:
        print("GPU device(s):", ", ".join(sorted(df["gpu_device"].unique())))

    print_resolution_breakdown(df)
    print_matched_altitude_ratios(df)
    print_knockout_summary(df)
    print_correlations(df)
    make_plots(df, script_dir / "analysis_output")


if __name__ == "__main__":
    main()
