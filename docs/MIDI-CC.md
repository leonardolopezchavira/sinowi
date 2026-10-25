# MIDI CC reference

Sinowi responds to MIDI Control Change (CC) messages the way the Hydrasynth
does: the firmware's own CC chart (from the Hydrasynth owner's manual,
firmware 2.2.0) controls the synth's parameters. Sinowi adds two CCs of its
own for panel buttons, and adapts a few others so they work with an ordinary
0-127 controller knob.

## Before you start

- **Param RX must be set to CC.** On the panel: SYSTEM, then DOWN to page 6
  (MIDI), and turn the encoder under *Param RX* to *CC*. New Sinowi instances
  start with CC; instruments made earlier, like the real Hydrasynth, start
  with Param RX off.
- **MIDI channel:** the synth listens on the channel set as *MIDI RX* on
  System page 4 (default: Omni, all channels). Sinowi's own CCs (SHIFT and
  RANDOM) work on any channel.
- In a DAW, the plugin's automatable parameters are the same CCs, named after
  the parameter with its CC number, e.g. *Filter 1 Cutoff (CC 74)*.

## Panel buttons (Sinowi only)

Sinowi uses the only two CC numbers the Hydrasynth's chart leaves free for
two panel buttons. Set your controller's buttons to **momentary** (127 when
pressed, 0 when released): the button is held while the value is 64 or more.
These CCs go to Sinowi only, not to the synth (so a patch using CC 2 or 4 as a
mod matrix source does not receive them).

| CC | Panel button | Computer key |
|---|---|---|
| 2 | RANDOM | R |
| 4 | SHIFT | Shift |

The computer keys work while the mouse pointer is over the Sinowi panel (so
typing elsewhere does not press them); R is ignored while Command, Control or
Option is held. A button stays held while any of the mouse, its key or its CC
holds it, and the panel shows it pressed.

## Arpeggiator CCs: scaled to a full knob turn

The firmware reads these CCs as the parameter's own value (a list position or
a percentage) and ignores out-of-range values, so most of a normal knob's
0-127 travel would do nothing. Sinowi spreads 0-127 over the range the firmware
accepts:

| CC | Parameter | A full 0-127 turn covers |
|---|---|---|
| 106 | ARP Division | 0-11: 1/1, 1/2, 1/4, 1/8, 1/16, 1/32, then 1/1T ... 1/32T |
| 107 | ARP Gate | 5-100 % |
| 108 | ARP Mode | 0-7: Up, Down, Up/Dn, Up&Dn, Order, Random, Chord, Phrase |
| 109 | ARP Ratchet | 0-32 |
| 110 | ARP Chance | 0-100 % |
| 120 | ARP Octave | 1-4 |
| 122 | ARP Length | 0-32 (0 = Default) |

## The panel follows these CCs

The filter knobs and arpeggiator switches on Sinowi's panel turn to match
incoming CCs (only the drawing moves; the synth sees the CC):

- Filter 1 selected: CC 74 Cutoff, CC 71 Resonance, CC 50 Drive
- Filter 2 selected: CC 55 Cutoff, CC 56 Resonance (Morph has no CC)
- Arpeggiator: CC 108 Mode, CC 120 Octave, CC 106 Division (the switch has
  only 1/4, 1/8, 1/16 and 1/32; other divisions change the sound but not the
  switch)

## All 128 CC numbers

"Module" and "Parameter" are as in the Hydrasynth owner's manual (firmware
2.2.0). Any CC can also be chosen as a modulation source in a patch's mod
matrix (*MIDI CC 000-127*).

| CC | Module | Parameter | Notes |
|---|---|---|---|
| 0 | System | Bank select MSB |  |
| 1 | System | Modulation wheel |  |
| 2 | **Sinowi** | **RANDOM button** | Held while the value is 64 or more. Not passed to the synth. |
| 3 | Mixer | Noise Vol |  |
| 4 | **Sinowi** | **SHIFT button** | Held while the value is 64 or more. Not passed to the synth. |
| 5 | Voice | GlidTime |  |
| 6 | — | (not used) | Data Entry MSB: part of the NRPN messages the Hydrasynth uses |
| 7 | System | Master Volume |  |
| 8 | Mixer | Noise Pan |  |
| 9 | Mixer | Ring Mod Vol |  |
| 10 | Mixer | Ring Mod Pan |  |
| 11 | System | Expression pedal |  |
| 12 | Pre-fx | PRE-FX Param1 |  |
| 13 | Pre-fx | PRE-FX Param2 |  |
| 14 | Delay | Delay Feedback |  |
| 15 | Delay | Delay Time |  |
| 16 | Macros | Macro 1 |  |
| 17 | Macros | Macro 2 |  |
| 18 | Macros | Macro 3 |  |
| 19 | Macros | Macro 4 |  |
| 20 | Macros | Macro 5 |  |
| 21 | Macros | Macro 6 |  |
| 22 | Macros | Macro 7 |  |
| 23 | Macros | Macro 8 |  |
| 24 | OSC 1 | OSC1 wavscan |  |
| 25 | ENV 4 | ENV4 Attack |  |
| 26 | OSC 2 | OSC2 WavScan |  |
| 27 | ENV 4 | ENV4 Decay |  |
| 28 | LFO 2 | LFO2 Gain |  |
| 29 | Mutator 1 | Mutator1 Ratio |  |
| 30 | Mutator 1 | Mutator1 Depth |  |
| 31 | Mutator 1 | Mutator1 Dry/Wet |  |
| 32 | System | Bank select LSB |  |
| 33 | Mutator 2 | Mutator2 Ratio |  |
| 34 | Mutator 2 | Mutator2 Depth |  |
| 35 | Mutator 2 | Mutator2 Dry/Wet |  |
| 36 | Mutator 3 | Mutator3 Ratio |  |
| 37 | Mutator 3 | Mutator3 Depth |  |
| 38 | — | (not used) | Data Entry LSB: part of NRPN messages |
| 39 | Mutator 3 | Mutator3 Dry/Wet |  |
| 40 | Mutator 4 | Mutator4 Ratio |  |
| 41 | Mutator 4 | Mutator4 Depth |  |
| 42 | Mutator 4 | Mutator4 Dry/Wet |  |
| 43 | Mixer | RM12 Depth |  |
| 44 | Mixer | OSC1 Vol |  |
| 45 | Mixer | OSC1 Pan |  |
| 46 | Mixer | OSC2 Vol |  |
| 47 | Mixer | OSC2 Pan |  |
| 48 | Mixer | OSC3 Vol |  |
| 49 | Mixer | OSC3 Pan |  |
| 50 | Filter 1 | Filter1 Drive | Moves the panel's DRIVE / MORPH knob (Filter 1 selected) |
| 51 | Filter 1 | Filter1 Keytrack |  |
| 52 | Filter 1 | Filter1 LFO1amt |  |
| 53 | Filter 1 | Filter1 Vel Env |  |
| 54 | Filter 1 | Filter1 ENV1amt |  |
| 55 | Filter 2 | Flt2 Cutoff | Moves the panel's CUTOFF knob (Filter 2 selected) |
| 56 | Filter 2 | Flt2 Res | Moves the panel's RESONANCE knob (Filter 2 selected) |
| 57 | Filter 2 | Flt2 Type |  |
| 58 | Filter 2 | Filter2 Keytrack |  |
| 59 | Filter 2 | Filter2 LFO1amt |  |
| 60 | Filter 2 | Filter2 Vel Env |  |
| 61 | Filter 2 | Filter2 ENV1amt |  |
| 62 | Amp | Amp LFO2amt |  |
| 63 | Delay | Delay Wet tone |  |
| 64 | System | Sustain pedal |  |
| 65 | Reverb | Reverb Time |  |
| 66 | Voice | Glide |  |
| 67 | Reverb | Reverb Tone |  |
| 68 | Post-fx | POST-FX Param1 |  |
| 69 | Post-fx | POST-FX Param2 |  |
| 70 | LFO 1 | LFO1 Gain |  |
| 71 | Filter 1 | Filter1 Res | Moves the panel's RESONANCE knob (Filter 1 selected) |
| 72 | LFO 1 | LFO1 Rate |  |
| 73 | LFO 2 | LFO2 Rate |  |
| 74 | Filter 1 | Filter1 Cutoff | Moves the panel's CUTOFF knob (Filter 1 selected) |
| 75 | LFO 3 | LFO3 Gain |  |
| 76 | LFO 3 | LFO3 Rate |  |
| 77 | LFO 4 | LFO4 Gain |  |
| 78 | LFO 4 | LFO4 Rate |  |
| 79 | LFO 5 | LFO5 Gain |  |
| 80 | LFO 5 | LFO5 Rate |  |
| 81 | ENV 1 | ENV1 Attack |  |
| 82 | ENV 1 | ENV1 Decay |  |
| 83 | ENV 1 | ENV1 Sustain |  |
| 84 | ENV 1 | ENV1 Release |  |
| 85 | ENV 2 | ENV2 Attack |  |
| 86 | ENV 2 | ENV2 Decay |  |
| 87 | ENV 2 | ENV2 Sustain |  |
| 88 | ENV 2 | ENV2 Release |  |
| 89 | ENV 3 | ENV3 Attack |  |
| 90 | ENV 3 | ENV3 Decay |  |
| 91 | Reverb | Reverb Dry/Wet |  |
| 92 | Delay | Delay Dry/Wet |  |
| 93 | Pre-fx | PRE-FX Mix |  |
| 94 | Post-fx | POST FX Mix |  |
| 95 | Voice | Detune |  |
| 96 | ENV 3 | ENV3 Sustain |  |
| 97 | ENV 3 | ENV3 Release |  |
| 98 | — | (not used) | NRPN LSB: part of NRPN messages |
| 99 | — | (not used) | NRPN MSB: part of NRPN messages |
| 100 | — | (not used) | RPN LSB (MIDI standard) |
| 101 | — | (not used) | RPN MSB (MIDI standard) |
| 102 | ENV 5 | ENV5 Attack |  |
| 103 | ENV 5 | ENV5 Decay |  |
| 104 | ENV 5 | ENV5 Sustain |  |
| 105 | ENV 5 | ENV5 Release |  |
| 106 | ARP | ARP Division | Sinowi scales 0-127 onto 0-11: 1/1, 1/2, 1/4, 1/8, 1/16, 1/32, then 1/1T ... 1/32T; Moves the panel's DIVISION switch (1/4-1/32 only) |
| 107 | ARP | ARP Gate | Sinowi scales 0-127 onto 5-100 % |
| 108 | ARP | ARP Mode | Sinowi scales 0-127 onto 0-7: Up, Down, Up/Dn, Up&Dn, Order, Random, Chord, Phrase; Moves the panel's MODE switch |
| 109 | ARP | ARP Ratchet | Sinowi scales 0-127 onto 0-32 |
| 110 | ARP | ARP Chance | Sinowi scales 0-127 onto 0-100 % |
| 111 | OSC 1 | OSC1 Cent |  |
| 112 | OSC 2 | OSC2 Cent |  |
| 113 | OSC 3 | OSC3 Cent |  |
| 114 | Mixer | OSC3 FRate |  |
| 115 | Mixer | Noise FRate |  |
| 116 | Mixer | RM12 FRate |  |
| 117 | Voice | StWidth |  |
| 118 | Mixer | OSC1 FRate |  |
| 119 | Mixer | OSC2 FRate |  |
| 120 | ARP | ARP Octave | Sinowi scales 0-127 onto 1-4; Moves the panel's OCTAVE switch |
| 121 | — | (not used) | Reset All Controllers (MIDI channel-mode message) |
| 122 | ARP | ARP Length | Sinowi scales 0-127 onto 0-32 (0 = Default) |
| 123 | System | All notes off |  |
| 124 | ENV 4 | ENV4 Release |  |
| 125 | ENV 4 | ENV4 Sustain |  |
| 126 | — | (not used) | Mono Mode On (MIDI channel-mode message) |
| 127 | — | (not used) | Poly Mode On (MIDI channel-mode message) |

Hydrasynth is a trademark of Ashun Sound Machines (ASM). The CC chart is the
Hydrasynth's, from ASM's owner's manual; Sinowi is not affiliated with ASM.
