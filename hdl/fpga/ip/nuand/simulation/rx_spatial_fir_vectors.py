#!/usr/bin/env python3
"""Generate rx_spatial_fir_tb vectors from the bit-true golden model.

Golden model: sdr-scanner rfcore/spatial_fir.py (apply_fixed, quantize,
identity, fit_fir, normalize). Run with the sdr-scanner interpreter:

    ~/projects/sdr-scanner/.venv/bin/python rx_spatial_fir_vectors.py \
        --sdr-scanner ~/projects/sdr-scanner --out rx_spatial_fir_vectors

Per test <name> the output directory holds (all integers, space separated):
  <name>.scr    line 1: N K NSEQ NEV INIT_BANK INIT_E0 INIT_E1 INIT_FORCE
                        EXP_SAT EXP_TAG EXP_IDENT   (-1 = not checked)
                line 2: NSEQ candidate indices, the order in which the
                        output must move through the candidates
                NEV lines: AT KIND A B (event fired at the strobe of input
                        sample AT; KIND 1 write bank file A into the shadow
                        bank, one word per cycle; 2 commit with tag A;
                        3 lane enables A B; 4 force_identity A)
  <name>.in     N lines: x0_i x0_q x1_i x1_q
  <name>.exp    N lines: for each candidate y0_i y0_q y1_i y1_q
  <name>.bankB  20 lines: re im (Q2.14), index (i*2 + j)*5 + k
Candidate "bypass" is the raw input delayed by the center tap (2 samples).
"""
import argparse
import sys
from pathlib import Path

import numpy as np

N = 600


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdr-scanner", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    sys.path.insert(0, str(a.sdr_scanner))
    from rfcore import spatial_fir as sf

    taps, c = sf.TAPS, sf.TAPS // 2
    assert taps == 5
    out = a.out
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20261019)

    def ci16(n, amp):
        return rng.integers(-amp, amp, 2 * n, endpoint=True).astype(np.int16)

    def bypass(x1, x2):
        d = 2 * c
        y = []
        for x in (x1, x2):
            z = np.zeros_like(x)
            z[d:] = x[:-d]
            y.append(z)
        return y

    def fir(hq, x1, x2):
        y1, y2, _ = sf.apply_fixed(hq, x1, x2)
        return [y1, y2]

    def lane_sat(hq, x1, x2):
        m = 0
        for i in range(2):
            h = hq.copy()
            h[1 - i] = 0
            if sf.apply_fixed(h, x1, x2)[2] > 0:
                m |= 1 << i
        return m

    def rand_bank():
        q = rng.integers(-32768, 32767, (2, 2, taps, 2), endpoint=True).astype(np.int16)
        # Q2.14 range limits must appear: -2.0 and +2.0 - 2^-14
        q[0, 0, 0, 0], q[0, 1, 4, 1], q[1, 0, 2, 0], q[1, 1, 3, 1] = -32768, 32767, 32767, -32768
        return q

    def fitted_bank():
        f = np.linspace(-9e6, 9e6, 10)
        W = np.empty((10, 2, 2), complex)
        for n, fn in enumerate(f):
            ph = np.exp(-2j * np.pi * fn * 30e-9)
            W[n] = [[1.0, -0.45 * ph + 0.2j], [-0.38 / ph, 0.9 + 0.1j * fn / 9e6]]
        h, _ = sf.normalize(sf.fit_fir(W, f, 23.04e6))
        return sf.quantize(h)

    def scaled_center(v):
        h = np.zeros((2, 2, taps), complex)
        h[:, :, c] = v
        return sf.quantize(h)

    def write(name, x1, x2, cands, seq, banks, init_bank, en, force, events,
              exp_sat=-1, exp_tag=-1, exp_ident=-1):
        n = len(x1) // 2
        with open(out / f"{name}.scr", "w") as fh:
            fh.write(f"{n} {len(cands)} {len(seq)} {len(events)} {init_bank} "
                     f"{en[0]} {en[1]} {force} {exp_sat} {exp_tag} {exp_ident}\n")
            fh.write(" ".join(str(s) for s in seq) + "\n")
            for ev in events:
                fh.write(" ".join(str(e) for e in ev) + "\n")
        np.savetxt(out / f"{name}.in",
                   np.stack([x1[0::2], x1[1::2], x2[0::2], x2[1::2]], 1), fmt="%d")
        cols = []
        for y1, y2 in cands:
            cols += [y1[0::2], y1[1::2], y2[0::2], y2[1::2]]
        np.savetxt(out / f"{name}.exp", np.stack(cols, 1), fmt="%d")
        for b, q in enumerate(banks):
            np.savetxt(out / f"{name}.bank{b}", q.reshape(20, 2), fmt="%d")

    ident = sf.identity()

    # T1 identity from reset, full int16 range incl. both extremes.
    x1, x2 = ci16(N, 32768), ci16(N, 32768)
    x1[:8] = [-32768, 32767, 32767, -32768, 0, -1, 1, -32768]
    x2[:8] = [32767, -32768, -1, 0, -32768, 32767, -32768, 32767]
    byp = bypass(x1, x2)
    small = [np.abs(b.astype(np.int32)) < 2048 for b in byp]
    g = fir(ident, x1, x2)
    assert all(np.array_equal(g[i][small[i]], byp[i][small[i]]) for i in range(2))
    write("t1_ident_reset", x1, x2, [byp], [0], [], -1, (1, 1), 0, [],
          exp_sat=0, exp_tag=0, exp_ident=1)

    # T1 force_identity level over a non-identity bank, released mid-stream.
    x1, x2 = ci16(N, 2047), ci16(N, 2047)
    hb = rand_bank()
    write("t1_force", x1, x2, [bypass(x1, x2), fir(hb, x1, x2)], [0, 1], [hb], 0,
          (1, 1), 1, [(300, 4, 0, 0)], exp_ident=0)

    # T3 random full-range coefficients, 12-bit and int16-extreme inputs.
    x1, x2 = ci16(N, 2048), ci16(N, 2048)
    x1[200:220] = 32767
    x2[300:320] = -32768
    hb = rand_bank()
    write("t3_rand", x1, x2, [fir(hb, x1, x2)], [0], [hb], 0, (1, 1), 0, [],
          exp_sat=lane_sat(hb, x1, x2), exp_ident=0)

    # T3 fitted (fit_fir + normalize + quantize), 12-bit inputs.
    x1, x2 = ci16(N, 1500), ci16(N, 1500)
    hb = fitted_bank()
    write("t3_fit", x1, x2, [fir(hb, x1, x2)], [0], [hb], 0, (1, 1), 0, [],
          exp_sat=lane_sat(hb, x1, x2), exp_ident=0)

    # T4 saturation: 1.99 on both inputs, full-scale x of both signs.
    blk = np.where((np.arange(N) // 100) % 2 == 0, 2047, -2048)
    x1 = np.empty(2 * N, np.int16)
    x1[0::2], x1[1::2] = blk, -1 - blk
    x2 = x1.copy()
    hs = scaled_center(1.99)
    write("t4_sat", x1, x2, [fir(hs, x1, x2)], [0], [hs], 0, (1, 1), 0, [],
          exp_sat=3, exp_ident=0)
    # Commit to a small bank clears the sticky; in-flight old-bank samples
    # must not set it again.
    hm = scaled_center(0.1)
    assert lane_sat(hm, x1, x2) == 0
    write("t4_sat_clear", x1, x2, [fir(hs, x1, x2), fir(hm, x1, x2)], [0, 1], [hs, hm], 0,
          (1, 1), 0, [(180, 1, 1, 0), (300, 2, 5, 0)], exp_sat=0, exp_tag=5, exp_ident=0)

    # T5 commit atomicity: A -> B -> identity -> A, shadow written while streaming.
    x1, x2 = ci16(N, 2047), ci16(N, 2047)
    ha, hb = rand_bank(), fitted_bank()
    write("t5_commit", x1, x2, [fir(ha, x1, x2), fir(hb, x1, x2), bypass(x1, x2)],
          [0, 1, 2, 0], [ha, hb, ident], 0, (1, 1), 0,
          [(90, 1, 1, 0), (180, 2, 7, 0), (240, 1, 2, 0), (300, 2, 9, 0),
           (360, 1, 0, 0), (420, 2, 11, 0)], exp_sat=-1, exp_tag=11, exp_ident=0)

    # T6 RX_X1 bypass with a non-identity bank loaded; enable toggled.
    x1, x2 = ci16(N, 2047), ci16(N, 2047)
    hb = fitted_bank()
    byp, ff = bypass(x1, x2), fir(hb, x1, x2)
    write("t6_x1_lane0", x1, x2, [byp], [0], [hb], 0, (1, 0), 0, [], exp_sat=0, exp_ident=0)
    write("t6_x1_lane1", x1, x2, [byp], [0], [hb], 0, (0, 1), 0, [], exp_sat=0, exp_ident=0)
    write("t6_toggle", x1, x2, [ff, byp], [0, 1, 0, 1, 0], [hb], 0, (1, 1), 0,
          [(120, 3, 1, 0), (240, 3, 1, 1), (360, 3, 0, 1), (480, 3, 1, 1)], exp_ident=0)


if __name__ == "__main__":
    main()
