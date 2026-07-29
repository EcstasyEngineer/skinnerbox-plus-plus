# Output contract

The normalized interface every reward channel implements. The point of this
document: adding a new output — a candy dispenser, a haptic device, an audio
processor — must require **zero changes** to the estimator or policy. You write
an adapter (or point the MCP adapter at a server), map the semantic intents to
something your channel can safely do, done.

## 1. Semantic layer (what the policy emits)

The policy never chooses device parameters. It emits two kinds of message:

**Phasic — `RewardIntent`** (discrete, on qualifying events):

```json
{
  "kind": "micro_reward | recovery_reward | session_summary",
  "confidence": 0.83,
  "dose": 0.55,
  "max_duration_ms": 8000,
  "reason": "flow_vi_reward",
  "withheld": false
}
```

- `dose` is an abstract 0–1 magnitude. It is **never** a hardware amplitude,
  volume, or current. Adapters map dose ranges onto their own pre-approved
  behaviors.
- `withheld: true` means the moment qualified but the policy chose silence
  (counterfactual sampling). Log it; never actuate it.
- `kind` values are append-only stable identifiers.

**Tonic — `AmbientState`** (continuous, ~1 Hz):

```json
{
  "flow": 0.71,
  "regime": "DRAFTING | FLOW | EDITING | STALL",
  "net_cpm": 96.0,
  "del_ratio": 0.18,
  "burst_s": 41.2,
  "idle_s": 0.8,
  "focus_losses": 1
}
```

These JSON forms are exactly what the log adapter writes (wrapped in
`{"event": "reward" | "snapshot", ...}`), so the session log doubles as the
reference stream for any external consumer.

## 2. In-process binding (C++)

`src/core/adapter.h`:

```cpp
class IOutputAdapter {
    const char* name() const;
    void ambient(const AmbientState&);   // tonic, every tick, must be cheap
    void deliver(const RewardIntent&);   // phasic, includes withheld intents
    void shutdown();                     // restore everything you touched
};
```

Rules: non-blocking on the caller's thread, fail toward off/neutral, sensory
channels ignore withheld intents.

## 3. MCP binding (how hardware plugs in)

Physical outputs do not get bespoke serial protocols in this codebase. The
planned `McpAdapter` is an MCP **client** speaking Streamable HTTP JSON-RPC
(`initialize`, `tools/list`, `tools/call`) to a local device backend — the same
protocol shape any MCP server already exposes. A device backend that manages
BLE toys, an e-stim box, or a candy dispenser plugs in by exposing
**preset-based tools**; this plugin calls them.

Delivery mapping (configurable, this is the shipped default):

| Intent | Backend call |
|---|---|
| `micro_reward`, dose < 0.5 | `tools/call` → preset `ACKNOWLEDGE_LOW` |
| `micro_reward`, dose ≥ 0.5 | `tools/call` → preset `REWARD_MEDIUM` |
| `recovery_reward` | `tools/call` → preset `REWARD_MEDIUM` |
| `session_summary` | `tools/call` → preset `SESSION_COMPLETE` |
| any `withheld: true` | no call, ever |

Tonic state is **not** streamed to hardware by default; if a backend opts in,
it receives at most one `ambient` call per 30 s.

### Safety invariants (non-negotiable)

The MCP adapter will enforce, and expects the backend to independently enforce:

- The plugin sends **named presets only** — never amplitude, waveform,
  duration, or escalation parameters. The backend maps presets to behaviors
  configured by the human, in advance, out-of-band.
- The backend owns hard caps (max intensity, max duration, cooldown floor) and
  applies them regardless of what any client asks for.
- Heartbeat/watchdog: if the plugin stops calling, the backend returns to zero
  output on its own timeout. Failure state is always off.
- `max_duration_ms` is an upper bound the backend may shorten, never extend.
- Adapter-side rate limit at least as strict as the policy cooldown.
- Full event logging on both sides.

## 4. Adding a channel — checklist

1. Implement `IOutputAdapter` (in-process) **or** stand up an MCP server with
   preset tools (out-of-process).
2. Decide what each `kind` × dose range means for your channel. If the honest
   answer is "nothing safe," map it to nothing.
3. Ignore withheld intents unless you are a logger.
4. Make `shutdown()` (or your watchdog) restore the world.
5. Register in `start_engine()` (in-process) or in the INI (MCP endpoint).
