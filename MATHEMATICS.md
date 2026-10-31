# Room Theremin — Mathematical Model

This document records the exact acoustic and DSP equations used in both the HTML prototype and the C++20 research implementation.

---

## 1. Room geometry

Shoebox room with dimensions \(W\) (width), \(D\) (depth), \(H\) (height) in metres.

\[
V = W \cdot D \cdot H
\]

\[
S = 2(WD + WH + DH)
\]

**Listener** (fixed):

\[
\begin{align*}
l_x &= W/2 \\
l_y &= \mathrm{clamp}(0.18\,D,\; 0.8,\; 3.0) \\
l_z &= \min(1.5,\; 0.55\,H)
\end{align*}
\]

Listener faces \(+y\).

**Source** (movable, elevation locked):

\[
\begin{align*}
s_x &\in [0.3,\; W-0.3] \\
s_y &\in [l_y,\; D-0.3] \\
s_z &= l_z
\end{align*}
\]

Direct distance:

\[
d = \sqrt{(s_x-l_x)^2 + (s_y-l_y)^2 + (s_z-l_z)^2}
\]

---

## 2. Eyring reverberation time

Average absorption coefficient \(\alpha \in [0.03, 0.9]\).

\[
\mathrm{RT60} = -\frac{0.161\, V}{S \ln(1-\alpha)}
\qquad\text{(clamped to }[0.15, 12]\text{ s)}
\]

(The constant 0.161 arises from \(24\ln 10 / c\) with \(c \approx 343\,\mathrm{m\,s^{-1}}\).)

Mean free path:

\[
\mathrm{mfp} = \frac{4V}{S}
\]

Critical distance (direct = reverberant, \(Q=1\)):

\[
d_c = 0.057\sqrt{\frac{V}{\mathrm{RT60}}}
\]

Equivalent absorption area (Sabine):

\[
A_\mathrm{eq} = S\bigl(-\ln(1-\alpha)\bigr)
\]

---

## 3. Image-source early reflections

Reflection coefficient of a surface:

\[
\beta = \sqrt{1-\alpha}
\]

Image sources are generated for integer lattice indices \(n_x,n_y,n_z \in \{-2,\dots,2\}\) and parity bits \(q_x,q_y,q_z \in \{0,1\}\).  
Number of wall hits:

\[
r = |2n_x - q_x| + |2n_y - q_y| + |2n_z - q_z|
\]

Only \(1 \le r \le 3\) are retained.

Mirror coordinate of a source coordinate \(s\) across a wall of length \(L\):

\[
i = 
\begin{cases}
s + 2nL & q=0 \\
(2L-s) + 2nL & q=1
\end{cases}
\]

Path length from image to listener: \(d_i\).  
Amplitude:

\[
a = \frac{\beta^{r}}{d_i}
\]

Lateralization (equal-power stereo):

\[
\mathrm{lat} = \mathrm{clamp}\Bigl(\frac{\Delta x}{d_i},\;-1,\;1\Bigr),\qquad
\theta = (\mathrm{lat}+1)\frac{\pi}{4}
\]

\[
g_L = a\cos\theta,\qquad g_R = a\sin\theta
\]

Taps are sorted by \(a^2\) descending; the strongest 40 are kept.  
Pre-delay before the early-reflection ring:

\[
\tau_\mathrm{pre} = 0.6\,\frac{d}{c}
\]

When geometry changes, old and new tap sets are cross-faded with equal-power weights over 80 ms:

\[
w_\mathrm{old} = \cos\bigl(u\tfrac{\pi}{2}\bigr),\qquad
w_\mathrm{new} = \sin\bigl(u\tfrac{\pi}{2}\bigr),\qquad
u\in[0,1]
\]

---

## 4. Feedback Delay Network (Hall)

Eight delay lines. Base length set by mean free path:

\[
L_i = \mathrm{nearPrime}\Bigl(\frac{\mathrm{mfp}}{c}\cdot f_s\cdot f_i\Bigr)
\]

with relative factors

\[
f = [0.55,\;0.68,\;0.80,\;0.93,\;1.05,\;1.19,\;1.33,\;1.50]
\]

Per-line feedback gain for target RT60 (60 dB decay):

\[
g_i = 10^{-3 L_i / (f_s\cdot\mathrm{RT60})}
\]

One-pole low-pass coefficient in the feedback path (sample-rate aware):

\[
d_i = \mathrm{clamp}\Bigl(\alpha_\mathrm{damp}\cdot\frac{L_i}{f_s\cdot 0.025},\;0,\;0.95\Bigr)
\]

Input diffusion (sign pattern, gain 0.35):

\[
s = [+1,-1,+1,+1,-1,+1,-1,-1]
\]

Mixing matrix: orthonormal 8-point Walsh–Hadamard transform (scale \(1/\sqrt{8}\)).

Two stereo output rows (different sign patterns) are taken from the mixed vector and scaled by the diffuse-field normalisation

\[
\mathrm{tailAuto} = \sqrt{\frac{16\pi / A_\mathrm{eq}}{e_\mathrm{FDN}}}
\]

where the expected FDN energy factor is

\[
e_\mathrm{FDN} = 0.86\cdot 2\cdot 0.1225\cdot(8\cdot 0.1225)\cdot\frac{\mathrm{RT60}\,f_s}{13.8\cdot\overline{L}}
\]

Final wet contribution per channel: \(\mathrm{tailAuto}\cdot 0.35\cdot(\text{row output})\).

---

## 5. Dattorro plate

All delay lengths are taken from the classic 29 761 Hz reference design and scaled by

\[
k = \frac{f_s}{29761}
\]

| Section | Lengths (samples @ 29 761 Hz) | Coefficients |
|---------|-------------------------------|--------------|
| Input AP | 142, 107, 379, 277 | 0.75, 0.75, 0.625, 0.625 |
| Mod AP  | 672, 908 | −0.7 |
| Tank d1 | 4453, 4217 | — |
| Tank AP2| 1800, 2656 | \(\mathrm{clamp}(\mathrm{decay}+0.15,0.25,0.5)\) |
| Tank d2 | 3720, 3163 | — |

Output uses Dattorro’s seven-tap sets per channel.  
Wet gain is obtained from a lookup table on the decay parameter:

\[
\begin{align*}
&(0.10,\;1.20),\ (0.40,\;1.93),\ (0.62,\;2.45),\\
&(0.85,\;4.06),\ (0.97,\;9.00)
\end{align*}
\]

linearly interpolated, then

\[
g_\mathrm{wet} = 0.3\sqrt{\frac{2.45}{\mathrm{scale}(\mathrm{decay})}}
\]

The plate **ignores** room size; position and HRTF/pan still apply.

---

## 6. Dry-path coupling

**Air absorption** (stylistic one-pole / biquad low-pass):

\[
f_c = \mathrm{clamp}\Bigl(\frac{16000}{1+0.09\,d},\;2800,\;16000\Bigr)\ \mathrm{Hz}
\]

**Strongest first-order wall tap**  
Among the six first-order images the largest amplitude ratio

\[
\rho = \beta\cdot\frac{d}{d_i}
\]

is selected. Delay and gain mixed into the dry bus:

\[
\tau_\mathrm{wall} = \mathrm{clamp}\Bigl(\frac{d_i-d}{c},\;0.0003,\;0.19\Bigr)\ \mathrm{s},
\qquad
g_\mathrm{wall} = 0.55\,\rho
\]

**Theremin source softening**

\[
f_c^\mathrm{voice} = \mathrm{clamp}\Biggl(
\frac{f_0\cdot 4.2\cdot\bigl(0.68+0.32\sqrt{\mathrm{level}}\bigr)}{1+0.05\,d},\;
380,\;8600\Biggr)\ \mathrm{Hz}
\]

**Distance amplitude** (panner):

\[
g = \frac{1}{\max(0.5,\,d)}
\]

---

## 7. Binaural panner (baseline)

Azimuth (source relative to listener facing \(+y\)):

\[
\mathrm{az} = \operatorname{atan2}(s_x-l_x,\; s_y-l_y)
\]

Equal-power ILD:

\[
\mathrm{pan} = \sin(\mathrm{az}),\qquad
g_L = g\sqrt{\tfrac12(1-\mathrm{pan})},\qquad
g_R = g\sqrt{\tfrac12(1+\mathrm{pan})}
\]

Woodworth ITD (seconds, positive delay on the far ear):

\[
\Delta t = 0.0875\cdot\frac{\sin(\mathrm{az})}{c}
\]

(An HRTF backend can replace the ILD+ITD block without changing the engine interface.)

---

## 8. Master chain

\[
\begin{align*}
\mathrm{dry} &\leftarrow \mathrm{airLP}(\mathrm{source}) + g_\mathrm{wall}\cdot\mathrm{Delay}(\mathrm{airLP}) \\
(\mathrm{L},\mathrm{R}) &\leftarrow \mathrm{Panner}(\mathrm{dry}) \\
\mathrm{wet} &\leftarrow 
\begin{cases}
\mathrm{Hall}(\mathrm{dry}) + E\cdot\mathrm{Early}(\mathrm{dry}) & \text{hall}\\
\mathrm{Plate}(\mathrm{dry}) & \text{plate}\\
0 & \text{off}
\end{cases} \\
\mathrm{out} &\leftarrow \mathrm{Compressor}\bigl(L_\mathrm{master}^2\cdot(\mathrm{pan}+\mathrm{mix}\cdot\mathrm{wet})\bigr)
\end{align*}
\]

Compressor: soft-knee threshold −14 dB, ratio 3 : 1, attack ≈ 1 ms, release ≈ 100 ms.

---

## 9. Units & constants

| Symbol | Value | Meaning |
|--------|-------|---------|
| \(c\) | 343 m s⁻¹ | speed of sound |
| \(f_s\) | 48 000 Hz (default) | sample rate |
| \(k\) | \(f_s/29761\) | Dattorro delay scale |
| \(\alpha\) | 0.03 … 0.9 | average absorption |

All coefficient arithmetic is performed in double precision; audio buffers are float.

---

*These formulae are exactly those realised in `Room Theremin.html` (AudioWorklet) and `room_theremin.cpp`.*
