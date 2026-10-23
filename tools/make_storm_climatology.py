"""Bake the thunderstorm climatology for the weather events (src/simulations/StormClimatology.h).

Source: the NASA LIS/OTD gridded lightning climatology, version 2.3.2015 (GHRC DAAC):
  - LIS/OTD 0.5 Degree High Resolution Monthly Climatology (HRMC), LISOTD_HRMC_V2.3.2015.nc
    https://data.ghrc.earthdata.nasa.gov/ghrcw-protected/lohrmc__2.3.2015/LISOTD_HRMC_V2.3.2015.nc
  - LIS/OTD 2.5 Degree Low Resolution Diurnal Climatology (LRDC), LISOTD_LRDC_V2.3.2015.nc
    https://data.ghrc.earthdata.nasa.gov/ghrcw-protected/lolrdc__2.3.2015/LISOTD_LRDC_V2.3.2015.nc
Both need a (free) NASA Earthdata login to download; this script only reads local copies.
Citation (GHRC asks for it; also in THIRD_PARTY_NOTICES.txt):
  Cecil, D. J., 2006: LIS/OTD Gridded Lightning Climatology Data Collection, Version 2.3.2015. NASA Global
  Hydrology Resource Center DAAC, Huntsville, Alabama, U.S.A. doi:10.5067/LIS/LIS-OTD/DATA311
  Cecil, D. J., D. E. Buechler, R. J. Blakeslee, 2014: Gridded lightning climatology from TRMM-LIS and
  OTD: Dataset description. Atmos. Res., 135-136, 404-414, doi:10.1016/j.atmosres.2012.06.028
NASA data: public domain.

Usage:
  python tools/make_storm_climatology.py --hrmc LISOTD_HRMC_V2.3.2015.nc --lrdc LISOTD_LRDC_V2.3.2015.nc
         [--out assets/textures/storm_climatology.bin] [--step 1.0]
Needs numpy and h5py (the .nc files are netCDF-4 = HDF5; netCDF4-python works too). Without the output
file the app uses StormClimatology's analytic model.

Output layout (little endian, read by StormClimatology::load):
  char[4]  "SLWX"
  uint32   version (1), nLon, nLat, nMonth, nHour, dLon, dLat
  float32  lon0, lat0, stepLon, stepLat          monthly grid: centre of cell [0][0] (west, south), degrees
  float32  dLon0, dLat0, dStepLon, dStepLat      diurnal grid, the same
  float32  monthly[nMonth][nLat][nLon]           flashes per km^2 per year (month m centred on day
                                                 (m + 0.5) 365.25 / 12; latitude ascending, longitude from
                                                 -180 eastward)
  float32  diurnal[dLat][dLon][nHour]            factor (mean 1 over the hours of each cell); bin h centred
                                                 on local solar hour (h + 0.5) 24 / nHour
"""
import argparse
import os
import struct
import sys

import numpy as np

YEAR_DAYS = 365.25


def open_nc(path):
    try:
        import h5py
        return 'h5py', h5py.File(path, 'r')
    except ImportError:
        pass
    try:
        import netCDF4
        ds = netCDF4.Dataset(path, 'r')
        ds.set_auto_mask(False)
        return 'netCDF4', ds
    except ImportError:
        sys.exit('need h5py or netCDF4 to read %s (pip install h5py)' % path)


def variables(kind, ds):
    if kind == 'h5py':
        out = {}
        ds.visititems(lambda n, o: out.__setitem__(n.split('/')[-1], o) if hasattr(o, 'shape') else None)
        return out
    return dict(ds.variables)


def attr(kind, var, name):
    try:
        v = var.attrs[name] if kind == 'h5py' else var.getncattr(name)
    except (KeyError, AttributeError):
        return None
    if isinstance(v, bytes):
        v = v.decode('latin-1')
    if isinstance(v, np.ndarray) and v.size == 1:
        v = v.item()
        if isinstance(v, bytes):
            v = v.decode('latin-1')
    return v


def read_rate(path, prefix, tsize):
    """The combined LIS+OTD flash rate as [time][lat][lon] (lat ascending, lon from -180), its units string."""
    kind, ds = open_nc(path)
    vs = variables(kind, ds)
    names = [n for n in vs if n.upper().startswith(prefix) and n.upper().endswith('COM_FR')]
    if not names:
        names = [n for n in vs if 'COM_FR' in n.upper()]
    if not names:
        sys.exit('%s: no *_COM_FR variable (have %s)' % (path, sorted(vs)))
    var = vs[names[0]]
    a = np.array(var[...], dtype=np.float64)
    fill = attr(kind, var, '_FillValue')
    if fill is not None:
        a[a == fill] = np.nan
    units = str(attr(kind, var, 'units') or '')
    if a.ndim != 3:
        sys.exit('%s: %s has shape %s, want 3 axes' % (path, names[0], a.shape))
    # Axis roles by size: time = tsize, latitude = half of longitude.
    shp = list(a.shape)
    t_ax = shp.index(tsize)
    rest = [i for i in range(3) if i != t_ax]
    lat_ax, lon_ax = (rest[0], rest[1]) if shp[rest[0]] * 2 == shp[rest[1]] else (rest[1], rest[0])
    if shp[lat_ax] * 2 != shp[lon_ax]:
        sys.exit('%s: cannot tell latitude from longitude in shape %s' % (path, a.shape))
    a = np.transpose(a, (t_ax, lat_ax, lon_ax))
    lat = next((np.array(vs[n][...], float).ravel() for n in vs if n.lower() in ('lat', 'latitude')), None)
    lon = next((np.array(vs[n][...], float).ravel() for n in vs if n.lower() in ('lon', 'longitude')), None)
    if lat is not None and lat.size == a.shape[1] and lat[0] > lat[-1]:
        a = a[:, ::-1, :]
    if lon is not None and lon.size == a.shape[2] and lon[0] > 0.0:
        # 0..360 grid: roll so that column 0 starts at -180.
        a = np.roll(a, -int(np.searchsorted(lon, 180.0)), axis=2)
    return a, units


def per_year(a, units, name):
    """To flashes per km^2 per year, the unit StormClimatology speaks."""
    u = units.lower().replace(' ', '')
    lat = np.linspace(-90, 90, a.shape[1] + 1)
    w = np.cos(np.radians(0.5 * (lat[1:] + lat[:-1])))[None, :, None] * np.ones_like(a)
    mean = np.nansum(a * w) / np.sum(w)
    if 'day' in u or 'd-1' in u or ('yr' not in u and 'year' not in u and mean < 0.2):
        print('%s: units %r (global mean %.4g): per day, x %.2f' % (name, units, mean, YEAR_DAYS))
        return a * YEAR_DAYS
    print('%s: units %r (global mean %.4g): per year' % (name, units, mean))
    return a


def box_sum(a, r):
    """Sum over (2r+1)^2 cells in the last two axes, longitude cyclic, latitude clamped."""
    out = np.zeros_like(a)
    for dy in range(-r, r + 1):
        sh = np.clip(np.arange(a.shape[-2]) + dy, 0, a.shape[-2] - 1)
        rows = a[..., sh, :]
        for dx in range(-r, r + 1):
            out += np.roll(rows, -dx, axis=-1)
    return out


def build_diurnal(lrdc):
    """Per-cell diurnal factors from the LRDC rates [hour][lat][lon] -> [lat][lon][hour], mean 1.

    The LRDC is noisy where few flashes were seen (oceans, high latitudes): the shape is the flash-weighted
    shape of a 3x3 neighbourhood, pulled toward its latitude band's shape by a prior worth ~2 fl/km^2/yr."""
    r = np.nan_to_num(np.maximum(lrdc, 0.0))
    nh, ny, nx = r.shape
    nb = box_sum(r, 1)                        # [hour][lat][lon]
    mb = nb.mean(axis=0)                      # neighbourhood mean rate, per cell
    # Latitude bands of +-10 degrees (4 cells of 2.5).
    band = np.zeros((nh, ny))
    for y in range(ny):
        y0, y1 = max(0, y - 4), min(ny, y + 5)
        band[:, y] = r[:, y0:y1, :].sum(axis=(1, 2))
    bm = band.mean(axis=0)
    zshape = np.where(bm[None] > 0, band / np.maximum(bm[None], 1e-12), 1.0)   # [hour][lat]
    prior = 2.0 * 9.0                                                         # 9 cells in the box
    f = (nb + prior * zshape[:, :, None]) / (mb[None] + prior)
    # Smooth in hour, [1 2 1]/4 cyclic.
    f = 0.25 * np.roll(f, 1, axis=0) + 0.5 * f + 0.25 * np.roll(f, -1, axis=0)
    f = np.clip(f, 0.03, 8.0)
    f /= f.mean(axis=0, keepdims=True)
    return np.transpose(f, (1, 2, 0)).astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hrmc', required=True)
    ap.add_argument('--lrdc', required=True)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'assets', 'textures',
                                                   'storm_climatology.bin'))
    ap.add_argument('--step', type=float, default=1.0, help='monthly grid step, degrees (multiple of 0.5)')
    args = ap.parse_args()

    hrmc, u1 = read_rate(args.hrmc, 'HRMC', 12)
    hrmc = per_year(np.nan_to_num(np.maximum(hrmc, 0.0)), u1, 'HRMC')
    k = int(round(args.step / (180.0 / hrmc.shape[1])))
    if k > 1:
        nm, ny, nx = hrmc.shape
        hrmc = hrmc[:, :ny // k * k, :nx // k * k].reshape(nm, ny // k, k, nx // k, k).mean(axis=(2, 4))
    nm, ny, nx = hrmc.shape
    step_lat, step_lon = 180.0 / ny, 360.0 / nx

    lrdc, u2 = read_rate(args.lrdc, 'LRDC', 24)
    diurnal = build_diurnal(lrdc)
    dny, dnx, nh = diurnal.shape
    dstep_lat, dstep_lon = 180.0 / dny, 360.0 / dnx

    lat = -90 + step_lat * (np.arange(ny) + 0.5)
    w = np.cos(np.radians(lat))[None, :, None]
    gmean = float((hrmc * w).sum() / (w.sum() * nm * nx))
    print('monthly grid %d x %d x %d (%.2f deg), diurnal %d x %d x %d (%.2f deg); global mean %.3f fl/km^2/yr, '
          'max %.1f' % (nx, ny, nm, step_lon, dnx, dny, nh, dstep_lon, gmean, hrmc.max()))

    with open(args.out, 'wb') as f:
        f.write(b'SLWX')
        f.write(struct.pack('<7I', 1, nx, ny, nm, nh, dnx, dny))
        f.write(struct.pack('<8f', -180 + 0.5 * step_lon, -90 + 0.5 * step_lat, step_lon, step_lat,
                            -180 + 0.5 * dstep_lon, -90 + 0.5 * dstep_lat, dstep_lon, dstep_lat))
        f.write(hrmc.astype('<f4').tobytes())
        f.write(diurnal.astype('<f4').tobytes())
    print('wrote %s (%.2f MB)' % (args.out, os.path.getsize(args.out) / 1e6))


if __name__ == '__main__':
    main()
