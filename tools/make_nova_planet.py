#!/usr/bin/env python3
# Renders the title-screen pictures for examples/NOVA.BAS: a ringed gas giant
# (NOVAPLAN.PNG, 360x240) and a cratered moon (NOVAMOON.PNG, 80x80), ray-cast
# with lighting, limb darkening, the rings' shadow on the planet and the
# planet's shadow on the rings. Both are at the screen's own resolution, twice
# SCREEN 13's, and NOVA.BAS draws them with IMAGE into boxes half that size.
#   python3 tools/make_nova_planet.py [folder]
import os, sys, zlib, struct
import numpy as np

rng = np.random.default_rng(7)

def save_png(path, rgb):
    img = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w, _ = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    print(path, w, "x", h)

def noise(u, v, modes, scale_u, scale_v, seed):
    # Smooth noise from random Fourier modes, about -1..1.
    r = np.random.default_rng(seed)
    out = np.zeros_like(u)
    for _ in range(modes):
        ku, kv = r.normal(0, scale_u), r.normal(0, scale_v)
        out += np.sin(ku * u + kv * v + r.uniform(0, 2 * np.pi)) / modes ** 0.5
    return out

def unit(v):
    return v / np.linalg.norm(v)

def rot_x(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])

def rot_z(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])

def planet(W=360, H=240, cx=240, cy=150, R=96):
    # Camera: orthographic along -z. The planet frame is tilted so the rings
    # (its y = 0 plane) are seen from a little above, and rolled a little.
    M = rot_x(np.radians(-24)) @ rot_z(np.radians(16))
    light = unit(M @ unit(np.array([-0.75, 0.45, 0.55])))  # from the upper left, in front
    ys, xs = np.mgrid[0:H, 0:W].astype(float) + 0.5
    pts = np.stack([xs - cx, cy - ys, np.full_like(xs, 4 * R)], -1)
    o = pts @ M.T
    d = M @ np.array([0.0, 0.0, -1.0])
    # Sphere.
    b = o @ d
    c = (o * o).sum(-1) - R * R
    disc = b * b - c
    hit_s = disc > 0
    ts = np.where(hit_s, -b - np.sqrt(np.maximum(disc, 0)), np.inf)
    # Ring plane.
    tr = -o[..., 1] / d[1]
    q = o + tr[..., None] * d
    rr = np.hypot(q[..., 0], q[..., 2]) / R
    rin, rout = 1.3, 2.2
    hit_r = (rr > rin) & (rr < rout)

    def ring_density(r):
        t = (r - rin) / (rout - rin)
        dens = 0.62 + 0.18 * np.sin(t * 37) * np.sin(t * 11 + 1) + 0.06 * np.sin(t * 150)
        dens *= np.clip((t - 0.0) / 0.08, 0, 1) * np.clip((1 - t) / 0.05, 0, 1)
        dens *= np.where((t > 0.62) & (t < 0.68), 0.08, 1.0)   # the big gap
        dens *= np.where((t > 0.9) & (t < 0.915), 0.3, 1.0)
        dens *= np.where(t < 0.18, 0.35 + t * 2, 1.0)           # the faint inner ring
        return np.clip(dens, 0, 1)

    img = np.zeros((H, W, 3))
    # Planet surface.
    p = o + np.where(hit_s, ts, 0)[..., None] * d
    n = p / R
    lat = np.arcsin(np.clip(n[..., 1], -1, 1))
    lon = np.arctan2(n[..., 0], n[..., 2])
    warp = 0.035 * noise(lon, lat, 40, 3, 6, 1) + 0.012 * noise(lon, lat, 60, 9, 18, 2)
    band = lat * 7 + 0.6 * np.sin(lat * 23) + warp * 6
    pal = np.array([[0.93, 0.83, 0.62], [0.82, 0.62, 0.38], [0.96, 0.89, 0.72],
                    [0.70, 0.48, 0.28], [0.88, 0.74, 0.52], [0.62, 0.42, 0.26]])
    f = (np.sin(band) * 0.5 + 0.5) * (len(pal) - 1)
    i0 = np.clip(f.astype(int), 0, len(pal) - 2)
    fr = (f - i0)[..., None]
    col = pal[i0] * (1 - fr) + pal[i0 + 1] * fr
    col *= (0.96 + 0.05 * noise(lon, lat, 50, 20, 40, 3))[..., None]
    # A storm.
    sl, sb = lon - 0.55, lat + 0.38
    storm = np.exp(-((sl / 0.20) ** 2 + (sb / 0.07) ** 2))
    col = col * (1 - storm[..., None] * 0.7) + np.array([0.80, 0.36, 0.20]) * storm[..., None] * 0.7
    col = np.where((np.abs(lat) > 1.15)[..., None], col * 0.8 + np.array([0.45, 0.5, 0.55]) * 0.2, col)
    mu = np.clip(-(n @ d), 0, 1)  # how much the surface faces us
    diff = np.clip(n @ light, 0, 1)
    # The rings' shadow on the planet.
    tl = -p[..., 1] / light[1]
    qs = p + tl[..., None] * light
    rs = np.hypot(qs[..., 0], qs[..., 2]) / R
    shadow = np.where((tl > 0) & (rs > rin) & (rs < rout), 1 - 0.85 * ring_density(rs), 1.0)
    shade = (0.03 + 0.97 * diff * shadow) * (0.55 + 0.45 * mu ** 0.5)
    surf = col * shade[..., None]
    rim = (1 - mu) ** 3 * np.clip(diff * 1.6, 0, 1)
    surf += np.array([0.35, 0.45, 0.65]) * rim[..., None] * 0.35
    img = np.where(hit_s[..., None], surf, img)
    # Rings, in front of the planet or where it isn't.
    front = hit_r & (~hit_s | (tr < ts))
    dens = ring_density(rr)
    t = (rr - rin) / (rout - rin)
    rcol = np.array([0.86, 0.78, 0.62]) * (0.85 + 0.15 * np.sin(t * 40))[..., None]
    # The planet's shadow on the rings.
    b2 = q @ light
    c2 = (q * q).sum(-1) - R * R
    in_shadow = (b2 * b2 - c2 > 0) & (b2 < 0)
    lit = np.where(in_shadow, 0.05, 0.35 + 0.65 * abs(light[1]) * 2.2)
    ring = rcol * (dens * lit)[..., None]
    a = (dens * 0.95)[..., None]
    img = np.where(front[..., None], img * (1 - a) + ring, img)
    return img

def moon(S=80, R=36):
    light = unit(np.array([-0.75, 0.45, 0.55]))
    ys, xs = np.mgrid[0:S, 0:S].astype(float) + 0.5
    x, y = (xs - S / 2) / R, (S / 2 - ys) / R
    r2 = x * x + y * y
    inside = r2 < 1
    z = np.sqrt(np.clip(1 - r2, 0, 1))
    n = np.stack([x, y, z], -1)
    h = 0.06 * noise(x, y, 60, 6, 6, 11)
    for _ in range(26):  # craters: a dip with a raised rim
        c = rng.uniform(-0.9, 0.9, 2)
        rad = rng.uniform(0.05, 0.22)
        dist = np.hypot(x - c[0], y - c[1]) / rad
        h += np.where(dist < 1, -0.08 * (1 - dist ** 2), 0) + 0.03 * np.exp(-((dist - 1) / 0.15) ** 2)
    gy, gx = np.gradient(h)
    nn = n.copy()
    nn[..., 0] -= gx * 6
    nn[..., 1] += gy * 6
    nn /= np.linalg.norm(nn, axis=-1, keepdims=True)
    diff = np.clip(nn @ light, 0, 1)
    albedo = 0.55 + 0.15 * noise(x, y, 30, 3, 3, 12) + h
    col = np.array([0.78, 0.76, 0.74]) * (albedo * (0.04 + 0.96 * diff))[..., None]
    return np.where(inside[..., None], col, 0)

out = sys.argv[1] if len(sys.argv) > 1 else "examples"
save_png(os.path.join(out, "NOVAPLAN.PNG"), planet())
save_png(os.path.join(out, "NOVAMOON.PNG"), moon())
