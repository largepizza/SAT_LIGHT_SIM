"""Readers for the files the app's tools write, shaped for plotting.

LobeDump   SatModelTool --dump-json (sat-light-sim-lobe-dump/1)
BenchRun   SatModelTool --run-benchmark report (sat-light-sim-benchrun/1)
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .geom import Mesh


def load_json(path) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


@dataclass
class Level:
    budget: int              # -1 = the exact merge (no budget)
    normals: np.ndarray      # (L,3) body frame
    area: np.ndarray         # (L,)
    alpha: np.ndarray        # (L,) effective roughness incl. the merged normal spread
    group: np.ndarray        # (L,)
    tri_lobe: np.ndarray     # (N,) lobe index of every triangle
    raw: list


class LobeDump:
    def __init__(self, path):
        d = load_json(path)
        if d.get("format") != "sat-light-sim-lobe-dump/1":
            raise ValueError(f"{path}: not a lobe dump")
        self.raw = d
        self.id = d["model"]["id"]
        self.name = d["model"]["name"]
        self.materials = d["model"]["materials"]
        self.groups = d["model"]["groups"]
        t = d["triangles"]
        self.tris = np.array([x["p"] for x in t], float)
        self.normals = np.array([x["n"] for x in t], float)
        self.area = np.array([x["area"] for x in t], float)
        self.tri_group = np.array([x["group"] for x in t], int)
        self.tri_material = np.array([x["material"] for x in t], int)
        self.tri_component = np.array([x["component"] for x in t], int)
        self.mesh = Mesh(self.tris, self.normals)
        self.levels: list[Level] = []
        for lv in d["levels"]:
            L = lv["lobes"]
            tl = np.full(len(t), -1, int)
            for li, lobe in enumerate(L):
                tl[lobe["tris"]] = li
            self.levels.append(Level(lv["budget"], np.array([x["n"] for x in L], float),
                                     np.array([x["area"] for x in L], float),
                                     np.array([x["alpha"] for x in L], float),
                                     np.array([x["group"] for x in L], int), tl, L))
        v = d["validation"]
        self.val_phase = np.array([r["phase_deg"] for r in v])
        self.val_brute = np.array([r["brute"] for r in v])
        self.val_lobes = np.array([r["lobes"] for r in v])  # (S, levels)
        s = d["sphere"]
        self.sphere_sun = np.array(s["sun"])
        shape = (s["n_lat"], s["n_lon"])
        self.sphere_brute = np.array(s["brute"]).reshape(shape)
        sl = np.array(s["lobes"])
        self.sphere_lobes = sl.reshape((-1,) + shape) if sl.ndim == 2 else sl.reshape((1,) + shape)

    def material_colors(self):
        """Per-triangle colour from the material tint (lifted so dark materials stay readable)."""
        out = []
        for m in self.tri_material:
            c = np.array(self.materials[m]["color"], float)
            out.append(0.25 + 0.75 * np.clip(c, 0, 1))
        return out

    def lobe_parent(self, fine: int, coarse: int) -> np.ndarray:
        """For each lobe of level `fine`, the lobe of level `coarse` that absorbed it."""
        lf, lc = self.levels[fine], self.levels[coarse]
        out = np.zeros(len(lf.raw), int)
        for i, lobe in enumerate(lf.raw):
            out[i] = lc.tri_lobe[lobe["tris"][0]]
        return out

    def errors(self, level: int, floor_rel: float = 0.01) -> np.ndarray:
        """|Δmag| of a level's lobes against brute force, over configurations no fainter than 5 mag below
        the median (the app's validator's significance floor)."""
        b = self.val_brute
        lit = b > floor_rel * np.median(b[b > 0])
        return np.abs(2.5 * np.log10(np.maximum(self.val_lobes[lit, level], 1e-30) / b[lit]))


class BenchRun:
    def __init__(self, path):
        d = load_json(path)
        if d.get("format") != "sat-light-sim-benchrun/1":
            raise ValueError(f"{path}: not a benchmark run report")
        self.raw = d
        self.id = d["benchmark"]["id"]
        self.citation = d["benchmark"].get("citation", "")
        self.reference = d.get("reference", {})
        self.result = d.get("result", {})
        self.comparison = d.get("comparison", [])
        self.phase_curve = d.get("phase_curve", [])
        self.obs = self._table(d.get("reference_observations"))
        self.samples = self._table(d.get("samples"))

    @staticmethod
    def _table(t):
        if not t:
            return {}
        cols = t["columns"]
        rows = t["rows"]
        return {c: np.array([r[i] for r in rows], dtype=object) for i, c in enumerate(cols)}

    def metric(self, name):
        for c in self.comparison:
            if c.get("metric") == name:
                return c
        return None


def find(project_data: Path, pattern: str) -> Path:
    hits = sorted(project_data.glob(pattern))
    if not hits:
        raise FileNotFoundError(f"{project_data}/{pattern}: run `python -m vidkit data <project>` first")
    return hits[0]
