# fx-filter — a minimal HARP effect device

A resonant lowpass filter as a HARP §8.8 `audio.fx` device: the DAW streams a track into it,
it returns the filtered (wet) signal, and it gets everything a HARP device gets — automation
lanes, sample-accurate parameter changes, total recall, front-panel echo — from harp's own
device code. The effect-specific part is one file, [`fx_filter.c`](fx_filter.c) (~330 lines
including comments).

Start here if you are building an effect. For an instrument, the reference device's
[`device/engine.c`](../../device/engine.c) is the worked example; it implements the same seam.

## Build and run

```sh
cmake -B build -S . && cmake --build build --target harp-fx-filter harp-probe
cmake -B build-vst -S tools/vst3-host && cmake --build build-vst --target harp-fx-shell harp-vst3-host

./build/harp-fx-filter --port 17990 --state-dir fx-state &      # the "hardware", on localhost
./build/harp-probe -d 127.0.0.1:17990 identify                  # caps include audio.fx

# bounce a tone through it, sweeping the cutoff, as a DAW insert would
HARP_ETH_DEVICE=127.0.0.1:17990 ./build-vst/harp-vst3-host \
    build-vst/VST3/Release/harp-fx-shell.vst3 \
    --input sine:1000 --ramp 1=0.1:0.9 --seconds 2 --out swept.wav
```

`harp-fx-shell` is the effect plugin (VST3; stereo in, stereo out, a host-side dry/wet Mix).
Installed into a DAW it binds the device named by `HARP_ETH_DEVICE`, or discovers one over
mDNS when the device runs with `--mdns`.

## How an effect plugs in

`harp-deviced` is harp's device daemon: protocol session, content-addressed state and recall,
the USB and Ethernet transports, and the audio render loop. It reaches the sound engine only
through the seam declared in [`device/device.h`](../../device/device.h). A device supplies
that seam in one translation unit and links the rest unchanged — the build target
`harp-fx-filter` in the top-level `CMakeLists.txt` is `harp-deviced` with `engine.c` swapped
for `fx_filter.c`.

What `fx_filter.c` implements:

| Part of the seam | What it is for |
|---|---|
| `engine_is_fx()` → 1 | Makes the device an **effect**: it advertises `audio.fx`, and the audio loop delivers the host's input audio to `render_output` in `a->fx_in`. |
| `render_output()` | Renders one block: reads the input columns (`a->fx_in`, `a->fx_in_n` frames each, in `a->in_slots` order) and writes **wet only**, interleaved, into the requested output slots. The host keeps the dry signal and does the mix; the dry never crosses the wire. |
| `g_params`, `param_index`, `engine_part_param_get/put` | The param map (§9.3) and its values. Ids, names and defaults are the device's identity for automation and recall. |
| `evq_push*`, `evq_full`, `evq_reset_for_new_stream` | The timestamped event queue (§9.2). The session pushes parameter sets, ramps and modulation; `render_output` applies each one **at the sample it names** by splitting the block there. That is what makes automation sample-accurate. |
| counters, meters, `engine_voices_*`, panic hooks | Diagnostics (§14), output meters (§9.9) and stream lifecycle. An effect has no voices, so several of these are one-liners. |

Rules the example follows, and yours should too:

- **Wet only.** Return the processed signal; never mix the dry in on the device.
- **Apply events at their timestamp.** Split the render at each pending event's `ts`, and
  count anything applied after its deadline in `g_evt_late` / `g_ramp_late` (they should stay 0).
- **Ramps move stored state.** A §9.4 ramp updates the parameter's stored value as it
  runs, so recall and the front-panel echo see automation, not just the audio.
- **Be deterministic.** No wall clock, randomness or uninitialised state in the render. The
  offline bounce is host-paced and must be byte-identical run to run; the tests check it.
- **Stay stable everywhere.** Every parameter value a DAW can send must be safe to render
  at any automation speed. The filter is a topology-preserving SVF for exactly that reason.
- **Report your pipeline.** If the engine's wet trails its input (a lookahead, a block FFT,
  a delay line inside the effect), report that depth as `device-pipeline-samples`, the
  host-paced `audio.start` response key 1 (§8.8). The device must still answer every pacing
  frame on time; only its content lags. The plugin adds the pipeline to the latency it reports
  when the device is connected at activation, and delays its dry to match. A device that
  connects later with a deeper pipeline is warned about, since the host was already told a
  latency without it (re-activating the plugin fixes that). `harp-fx-filter --pipeline N`
  simulates one; the tests use it.
- **Never go subnormal.** Recursive state decaying toward zero on a silent track passes
  through denormal floats, which are many times slower on x86. Flush tiny state to zero
  (as `svf_run` does) or set flush-to-zero on the render thread.

## Identity

`ENGINE_ID`, `ENGINE_VERSION` and `DEVICE_PRODUCT` are compile definitions on the target (as
is `NPARAMS`, the size of the param bank). The engine id and version are what recall checks
before restoring state onto a device (§12.2, §13.4). Bump the major version when a stored
state would sound different, and at least the minor version whenever the param map changes.

## Shipping a plugin for your effect

The FX shell reads its name, VST3 class UIDs, param table and input slots from
[`shell/shell_config.h`](../../shell/shell_config.h) (`HARP_FX_SHELL_*`; the defaults are this
example's). A product overrides them from its own header, outside the harp tree, and builds
its plugin from the same sources:

```sh
cmake -B build-vst -S tools/vst3-host \
    -DHARP_SHELL_VARIANT_KIND=fx \
    -DHARP_SHELL_VARIANT_NAME=my-fx-shell \
    -DHARP_SHELL_VARIANT_CONFIG=my_fx_config.h \
    -DHARP_SHELL_VARIANT_INCDIR=/path/to/dir/of/header
```

The param table must mirror the device's param map (ids and defaults). The UIDs must be
unique to your product and must never change once shipped.

## Testing

[`scripts/fx-filter-eth-test.sh`](../../scripts/fx-filter-eth-test.sh) drives this device
through the FX shell and checks, with no hardware:

- the input is processed and returned;
- an automated offline bounce is byte-identical run to run;
- automation reaches the device, moves its stored state and is never applied late;
- an automation point lands on the exact sample of the audio it was drawn against, at
  several DAW block sizes;
- save → mutate the device → reopen restores the params, archives the displaced state, and
  renders byte-identically;
- a front-panel knob echoes back to the DAW as automation;
- the wet arrives exactly the latency the plugin reports for delay compensation after its
  input, offline and live, and at Mix 50% the dry and wet line up;
- dense live automation is applied on time (no late events, no fence timeouts);
- after the device restarts mid-render, or only comes up after the render started, the
  live wet is sample-exact at the reported latency again.

Every check is deterministic on a loaded machine: no retries, no wall-clock luck (see the
"NO-FLAKE DESIGN" note at the top of the script).

It runs in CI on Linux, macOS and Windows as part of `scripts/eth-suite.sh`. Point it at your
own device and plugin (`FXDEVICED=… FXPLUG=…`) and adjust the parameter ids and expected
values to test yours.
