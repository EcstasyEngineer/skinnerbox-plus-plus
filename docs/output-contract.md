# Output contract

The normalized interface every reward channel implements. The point of this
document: adding a new output must require **zero changes** to the estimator
or policy. You write an adapter, map the semantic intents to something your
channel can safely do, done.

## 1. Semantic layer (what the policy emits)

The policy never chooses device parameters. It emits two kinds of message:

**Phasic — `RewardIntent`** (discrete, on qualifying events):

```json
{
  "kind": "micro_reward",
  "confidence": 0.83,
  "dose": 0.55,
  "max_duration_ms": 8000,
  "reason": "flow_vi_reward"
}
```

- `dose` is an abstract 0–1 magnitude. It is **never** a hardware amplitude,
  volume, or current. Adapters map dose ranges onto their own pre-approved
  behaviors under their own caps.
- `kind` values are append-only stable identifiers.

**Tonic — `AmbientState`** (continuous, ~1 Hz):

```json
{
  "flow": 0.71,
  "regime": "IDLE | TYPING | FLOW",
  "net_cpm": 96.0,
  "del_ratio": 0.18,
  "burst_s": 41.2,
  "idle_s": 0.8,
  "focus_losses": 1,
  "repetition": 0.31,
  "entropy": 4.12,
  "stall_frac": 0.0,
  "gate_ok": true
}
```

These JSON forms are what the log adapter writes (wrapped in
`{"event": "reward" | "snapshot", ...}`), so the session log doubles as the
reference stream for any external consumer.

## 2. In-process binding (C++)

`src/core/adapter.h`:

```cpp
class IOutputAdapter {
    const char* name() const;
    void ambient(const AmbientState&);   // tonic, every tick, must be cheap
    void deliver(const RewardIntent&);   // phasic, discrete rewards
    void shutdown();                     // restore everything you touched
};
```

Rules: non-blocking on the caller's thread; fail toward off/neutral.

## 3. Hardware binding: Intiface (the only hardware path)

`IntifaceAdapter` speaks Buttplug protocol v4 over a WebSocket to Intiface
Central (`ws://127.0.0.1:12345` by default). Intiface owns all device/BLE
support; this plugin is just a client.

The reward is **erogenous — the pleasure is the reinforcer**, and delivery is
shaped accordingly: ~200 ms ease-in, a sustain at fixed peak whose *length*
scales with dose (`buzz_ms * (0.5 + dose)`, capped at 4 s), ~300 ms ease-out.
Magnitude lives in duration, never intensity: inside a tight cap, intensity
differences aren't reliably discriminable, and habituation is fought with
timing, not escalation. Tonic flow is never sent to hardware — a continuous
baseline would adapt out and swamp the phasic reinforcer's contrast.

Safety invariants the adapter enforces (Buttplug provides none of these
server-side):

- **Client-side intensity ceiling** (`max_intensity`, default 0.30), applied
  in the device's own advertised step range. This is the only cap — there is
  no server-side one.
- **The adapter owns the stop.** `OutputCmd` has no duration; a value
  persists until changed. Every envelope ends in an explicit zero, and
  `shutdown()` sends `StopAllDevices`.
- **Dead-man's switch:** if the server advertises a nonzero `MaxPingTime`,
  the adapter pings at half that interval; if the plugin dies, pings stop and
  the server stops all devices itself.
- Tonic state is never streamed to hardware — rewards are phasic only.

## 4. Adding a channel — checklist

1. Implement `IOutputAdapter`.
2. Decide what each `kind` × dose range means for your channel. If the honest
   answer is "nothing safe," map it to nothing.
3. Make `shutdown()` restore the world.
4. Register in `start_engine()` (plugin) or the host's setup (demo).
