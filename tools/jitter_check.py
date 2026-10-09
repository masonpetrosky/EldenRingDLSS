"""Check whether ELDEN RING's camera motion matrix includes TAA jitter (values from capture f17976)."""
import numpy as np
np.set_printoptions(precision=7, suppress=False, linewidth=140)
P = np.array([[1.2634, 0, 0, 0], [0, 2.24604, 0, 0], [-0.000130208, -0.000154321, -5.00679e-06, 1], [0, 0, 0.0500673, 0]])
VP = np.array([[1.02972, -0.114904, 2.89e-06, -0.577216], [1.15041e-05, 2.23727, 4.42359e-07, -0.0883518],
               [0.73201, 0.161602, -4.0645e-06, 0.811798], [0, 0, 0.0500673, 0]])
VPprev = np.array([[1.03012, -0.114236, 2.88793e-06, -0.576802], [1.14412e-05, 2.23736, 4.4012e-07, -0.0879046],
                   [0.731456, 0.160844, -4.06622e-06, 0.81214], [0, 0, 0.0500673, 0]])
M_game = np.array([[1.00042, 0.000340039, -4.24684e-09, 0.000848216], [-0.000173167, 0.9998, 2.0017e-09, -0.000399797],
                   [0.649049, 0.567194, 0.999994, 1.30303], [-0.000460774, 0.000436872, 1.09139e-09, 0.999782]])
# Rows above are as RenderDoc printed them. Row-vector convention: clip = pos @ VP.
P_unjit = P.copy(); P_unjit[2, 0] = P_unjit[2, 1] = 0.0
for label, proj in (("jittered P", P), ("unjittered P", P_unjit)):
    V = VP @ np.linalg.inv(proj)
    R = V[:3, :3]
    print(f"V = VP @ inv({label}): orthonormality error {np.abs(R @ R.T - np.eye(3)).max():.3e}, "
          f"4th column {V[:, 3]}")
# The shader computes prev = M @ (u, v, d, 1) using M's rows, so M maps current UV+depth to previous UV.
S = np.array([[0.5, 0, 0, 0.5], [0, -0.5, 0, 0.5], [0, 0, 1, 0], [0, 0, 0, 1]])  # NDC -> UV
def cur_to_prev(vp_cur, vp_prev):
    m = S @ vp_prev.T @ np.linalg.inv(vp_cur.T) @ np.linalg.inv(S)
    return m / m[3, 3] * M_game[3, 3]
M_rebuilt = cur_to_prev(VP, VPprev)
print("max |M_game - M(VP, VPprev)| =", np.abs(M_game - M_rebuilt).max())
jx, jy = P[2, 0], P[2, 1]
print(f"current jitter: NDC ({jx:.6g}, {jy:.6g}) = pixels ({jx * 3840 / 2:+.4f}, {-jy * 2160 / 2:+.4f}) (x right, y down)")

print("\nM_game:\n", M_game)
V = VP @ np.linalg.inv(P)
# Hypotheses for how the engine builds the current->previous screen matrix.
cands = {}
for lab_cur, vp_c in (("cur jit", VP), ("cur unjit", V @ P_unjit)):
    for lab_prev, vp_p in (("prev as given", VPprev),):
        m = S @ vp_p.T @ np.linalg.inv(vp_c.T) @ np.linalg.inv(S)
        for lab_t, mm in (("", m), (" (transposed)", m.T)):
            mm = mm / mm[3, 3] * M_game[3, 3] if abs(mm[3, 3]) > 1e-12 else mm
            cands[f"{lab_cur}, {lab_prev}{lab_t}"] = mm
for k, mm in cands.items():
    print(f"{k:40s} max|diff| = {np.abs(mm - M_game).max():.3e}")
best = min(cands, key=lambda k: np.abs(cands[k] - M_game).max())
print("best:", best, "\n", cands[best])

print("\n--- shader uses rows 0,1,3 of the cbuffer matrix = rows of (S VPprev^T inv(VPcur^T) S^-1)^T ---")
rows = [0, 1, 3]
def build(vp_c, vp_p):
    m = (S @ vp_p.T @ np.linalg.inv(vp_c.T) @ np.linalg.inv(S)).T
    return m / m[3, 3] * M_game[3, 3]
P_prev_unjit_from = lambda vp_p: vp_p  # placeholder
# previous-frame jitter is unknown: solve for the NDC jitter (px, py) that best explains M_game when the
# previous matrix is unjittered (VPprev @ inv(Pprev_jit) @ P_unjit) and the current one is jittered or not.
def vp_with_jitter(vp, cur_jx, cur_jy, new_jx, new_jy):
    pj = P_unjit.copy(); pj[2, 0], pj[2, 1] = cur_jx, cur_jy
    pn = P_unjit.copy(); pn[2, 0], pn[2, 1] = new_jx, new_jy
    return vp @ np.linalg.inv(pj) @ pn
best = None
halton = lambda i, b: sum(((i // b**k) % b) / b**(k + 1) for k in range(12))
for cur_label, (cx, cy) in (("cur jittered", (jx, jy)), ("cur unjittered", (0.0, 0.0))):
    vp_c = vp_with_jitter(VP, jx, jy, cx, cy)
    for idx in range(0, 17):
        # candidate previous jitter in NDC from Halton(2,3), pixel -> NDC with y flipped; index 0 = no jitter
        if idx == 0:
            pjx = pjy = 0.0
        else:
            pjx = (halton(idx, 2) - 0.5) * 2 / 3840
            pjy = -(halton(idx, 3) - 0.5) * 2 / 2160
        for prev_label, (ax, ay) in (("prev keeps given", None), ("prev rejittered", (pjx, pjy))) if False else (("prev unjit->cand", (pjx, pjy)),):
            pass
        # Hypothesis A: VPprev as given was rendered with candidate jitter (pjx,pjy); engine used unjittered prev
        for prev_mode in ("given", "unjittered"):
            vp_p = VPprev if prev_mode == "given" else vp_with_jitter(VPprev, pjx, pjy, 0.0, 0.0)
            m = build(vp_c, vp_p)
            err = np.abs(m[rows] - M_game[rows]).max()
            if best is None or err < best[0]:
                best = (err, cur_label, prev_mode, idx)
            if idx in (0, 1, 2) or err < 2e-6:
                print(f"{cur_label:15s} prev {prev_mode:10s} cand-jitter idx {idx:2d}: max|diff| rows0,1,3 = {err:.3e}")
print("BEST:", best)
