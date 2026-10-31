# Room Theremin

**Interactive binaural room acoustics simulator**  
A theremin voice (or user-supplied WAV) placed inside a resizable shoebox room.  
The room geometry and absorption drive Eyring RT60, image-source early reflections, an 8-line feedback delay network (FDN) hall, a Dattorro plate, distance-dependent air absorption, a wall reflection, and a simple binaural panner.

**Authors**  
Lewis James Seals (theRealla) · Lawrence James Seals  

---

## Contents

| File | Description |
|------|-------------|
| `Room Theremin.html` | Self-contained Web Audio / AudioWorklet demo (browser) |
| `room_theremin.cpp`  | C++20 single-translation-unit research implementation |
| `MATHEMATICS.md`     | Full acoustic & DSP equations (Eyring, image-source, FDN, Dattorro, panner) |
| `README.md`          | This file |
| `LICENSE`            | MIT License |

See **[MATHEMATICS.md](MATHEMATICS.md)** for the complete set of formulae used by both the HTML and C++ implementations.

---

## Web demo (HTML)

Open `Room Theremin.html` in a modern browser (Chrome / Firefox / Edge).  
Use headphones for the binaural cue.

- Drag the pitch pad for frequency and level  
- Drag the source in the top-down plan view (or use the mouse wheel for distance)  
- Adjust room dimensions, absorption, reverb type (Hall / Plate / Off), and coupling  

The HTML is left completely unmodified from the original interactive prototype.

---

## C++20 research implementation

Single file, standard-library only (optional PulseAudio for live output).

```bash
# WAV-only build (any platform)
g++ -std=c++20 -O2 -pthread room_theremin.cpp -o room_theremin

# Live audio on Linux (PulseAudio)
g++ -std=c++20 -O2 -pthread -DROOM_THEREMIN_PULSE \
    room_theremin.cpp -o room_theremin -lpulse-simple -lpulse
```

### Usage

```bash
./room_theremin                       # interactive theremin
./room_theremin --wav loop.wav        # loop a WAV as source
./room_theremin --render out.wav      # render 20 s and exit
./room_theremin --render out.wav --duration 8
```

### Stdin commands (one per line)

```
W=12 D=16 H=6 A=0.25     # room width / depth / height (m), absorption
X=6 Y=8                  # source position (m)
T=440 V=0.66             # theremin pitch (Hz) / level (T=0 silences)
R=hall|plate|off         # reverb type
M=1.0 E=1.0 DM=0.25      # mix / early level / hall HF damping
PD=0.62 PDM=0.3 PP=12    # plate decay / damp / predelay (ms)
L=0.7 C=1                # master level / couple room into voice
P                        # print parameters + derived (RT60, dc, taps)
Q                        # quit
```

---

## Acoustic model (summary)

Full equations are in **[MATHEMATICS.md](MATHEMATICS.md)**. Brief overview:

- **Geometry** – shoebox room; listener fixed near the front wall; source free in the front hemisphere (elevation locked).
- **RT60** – Eyring formula  
  \( RT60 = -0.161\,V / (S\,\ln(1-\alpha)) \)
- **Mean free path** – \( mfp = 4V/S \)
- **Critical distance** – \( d_c = 0.057\sqrt{V/RT60} \)
- **Early reflections** – classic image-source method up to 3rd order, top taps kept, 80 ms equal-power crossfade on geometry change.
- **Hall** – 8-line FDN, near-prime delays scaled to mfp, orthonormal Walsh–Hadamard mixing, Sabine-based diffuse-field normalisation (`tailAuto`).
- **Plate** – fixed-topology Dattorro (1997) design, sample-rate-scaled delays; ignores room size but still responds to source position via the panner.
- **Dry path** – distance low-pass (air absorption proxy), strongest first-order wall tap, source softening, inverse-distance amplitude.
- **Panner** – equal-power ILD + Woodworth ITD (HRTF-swappable interface).

---

## References

### Core acoustics & reverb

1. Eyring, C. F. (1930). “Reverberation Time in ‘Dead’ Rooms.” *Journal of the Acoustical Society of America*, 1(2), 217–241.  
   – Origin of the Eyring RT60 formula used for the room.

2. Sabine, W. C. (1922). *Collected Papers on Acoustics*. Harvard University Press.  
   – Diffuse-field energy density and critical-distance relations.

3. Kuttruff, H. (2009). *Room Acoustics* (5th ed.). Spon Press.  
   – Image-source method, mean free path, statistical reverberation theory.

4. Jot, J.-M. & Chaigne, A. (1991). “Digital Delay Networks for Designing Artificial Reverberators.” *AES 90th Convention*, preprint 3030.  
   – Feedback Delay Network (FDN) architecture.

5. Rocchesso, D. & Smith, J. O. (1997). “Circulant and Elliptic Feedback Delay Networks for Artificial Reverberation.” *IEEE Transactions on Speech and Audio Processing*, 5(1), 51–63.  
   – Orthonormal mixing matrices (Walsh–Hadamard) for FDNs.

6. Dattorro, J. (1997). “Effect Design, Part 1: Reverberator and Other Filters.” *Journal of the Audio Engineering Society*, 45(9), 660–684.  
   – The classic plate topology implemented here.

7. Välimäki, V., Parker, J. D., Savioja, L., Smith, J. O. & Abel, J. S. (2012). “Fifty Years of Artificial Reverberation.” *IEEE Transactions on Audio, Speech, and Language Processing*, 20(5), 1421–1448.  
   – Comprehensive survey of FDN, plate, and image-source techniques.

### Spatial audio & binaural

8. Woodworth, R. S. & Schlosberg, H. (1954). *Experimental Psychology*. Holt.  
   – Woodworth ITD formula used in the baseline panner.

9. Gardner, W. G. & Martin, K. D. (1995). “HRTF Measurements of a KEMAR.” *Journal of the Acoustical Society of America*, 97(6), 3907–3908.  
   – Classic public-domain HRTF dataset (extension hook for the panner).

10. Begault, D. R. (1994). *3-D Sound for Virtual Reality and Multimedia*. Academic Press / NASA.  
    – Practical binaural rendering, distance cues, and early-reflection modelling.

### Implementation notes

11. Smith, J. O. (online). *Physical Audio Signal Processing*.  
    https://ccrma.stanford.edu/~jos/pasp/  
    – Delay-line, allpass, and FDN practical recipes.

12. Web Audio API specification (W3C).  
    https://www.w3.org/TR/webaudio/  
    – Basis of the original HTML/AudioWorklet prototype.

---

## Known limitations (documented intentionally)

- Source elevation is locked to listener height; no rear-hemisphere images by design.  
- FDN tail encodes volume, surface and absorption only — room *shape* appears solely in the early-tap pattern.  
- Absorption changes both decay *and* wet level (physically correct Sabine normalisation).  
- No frequency-dependent absorption α(f), no true air absorption, no diffraction/scattering/modes.  
- Default panner is ILD + ITD only (no HRTF dataset); front/back discrimination is therefore limited.  
- Plate delay lengths scale with sample rate (standard Dattorro port behaviour).

---

## License

MIT License (or as otherwise agreed by the authors).

Copyright © Lewis James Seals and Lawrence James Seals.

---

*Research prototype — for education, experimentation and further DSP development.*
