// Cloudflare Worker: relay between the sphere (assistant "Charlie" by default) and xAI Grok Voice (realtime speech to
// speech), plus an optional hand-off to the owner's own Grok Bot (webhook routine) for bigger jobs.
//
// One deployment per device: the device session lives in a Durable Object ("Device") so that a Grok Bot result
// arriving at /result can be pushed straight to the sphere over its open WebSocket (hibernation API: no cost idle).
//
// Cost model: xAI bills speech-to-speech per minute ($0.08/min grok-voice-3.0, checked 2026-11-25). The device's
// WebSocket to this Worker never touches xAI. An xAI session opens only when the device streams mic audio after a
// wake (or to read a result / say good morning) and closes as soon as the answer is done, after NO_SPEECH_SECONDS
// of silence, at MAX_SESSION_SECONDS, on a tap (cancel), or when the daily budget runs out.
//
// Device protocol (firmware/grok_sphere/relay_client.cpp):
//   device -> relay : binary = PCM16 LE mono 16 kHz mic audio
//                     text {"t":"hello","fw":".."} | {"t":"cancel"} | {"t":"greet","reason":"alarm","label":".."}
//                          {"t":"alarms","list":[{"time":"06:30","label":".."}]}   (device's current alarms)
//   relay  -> device: binary = PCM16 LE mono 24 kHz speech
//                     text {"state":"listening|thinking|speaking|idle"}, {"card":"clock"}, {"wx":"Sunny","f":72},
//                          {"chime":"result"}, {"alarm":{"op":"set|cancel","time":"06:30","label":".."}}
//
// Routes: GET /health, GET /usage (device token), GET /ws (device token, WebSocket),
//         POST /result (BOT_TOKEN: the owner's Grok Bot posts {request_id, status, spoken_text}).
import { DurableObject } from "cloudflare:workers";
import { persona, Who } from "./persona";

export interface Env {
  XAI_API_KEY: string; DEVICE_TOKENS: string;                       // secrets
  BOT_TOKEN?: string; GROKBOT_WEBHOOK_URL?: string; GROKBOT_WEBHOOK_KEY?: string;   // optional secrets (Grok Bot hand-off)
  XAI_MODEL: string; XAI_VOICE: string; XAI_REASONING: string;
  MAX_SESSION_SECONDS: string; NO_SPEECH_SECONDS: string; DAILY_MINUTES: string;
  HOME_LAT: string; HOME_LON: string; TIMEZONE: string; TASKS_PER_HOUR: string;
  ASSISTANT_NAME: string; OWNER_NAME: string; HOME_PLACE: string;
  BUDGET: DurableObjectNamespace<Budget>;   // legacy (v1), kept so the migration history stays valid
  DEVICE: DurableObjectNamespace<Device>;
}

const OUT_RATE = 24000;                 // xAI -> device speech (device upsamples 2x to 48 kHz)
const IN_RATE = 16000;                  // device mic -> xAI
const DEVICE_FRAME_BYTES = 2048;        // the ESP32 WebSocket client drops frames over ~15 KB
const LEAD_MS = 1200;                   // how far ahead of real time the sphere may be (its jitter buffer starts at 500 ms)
const MAX_TEXT = 2000;

function safeEqual(a: string, b: string): boolean {
  if (!a || !b) return false;
  const ea = new TextEncoder().encode(a), eb = new TextEncoder().encode(b);
  let d = ea.length ^ eb.length;
  for (let i = 0; i < Math.max(ea.length, eb.length); i++) d |= (ea[i] ?? 0) ^ (eb[i] ?? 0);
  return d === 0;
}
function deviceTokenOk(env: Env, t: string | null): boolean {
  return !!t && (env.DEVICE_TOKENS || "").split(",").map(s => s.trim()).filter(Boolean).some(x => safeEqual(x, t));
}
function bearer(req: Request): string {
  const h = req.headers.get("authorization") || "";
  return h.startsWith("Bearer ") ? h.slice(7).trim() : "";
}
function who(env: Env): Who {
  return { assistant: env.ASSISTANT_NAME || "Charlie", owner: env.OWNER_NAME || "the boss", place: env.HOME_PLACE || "home" };
}
function localNow(env: Env): string {
  return new Date().toLocaleString("en-US", { timeZone: env.TIMEZONE || "America/Chicago", weekday: "long", month: "long", day: "numeric", hour: "numeric", minute: "2-digit" });
}

// Legacy budget DO from v1 (no longer used; the Device DO keeps its own budget).
export class Budget extends DurableObject<Env> {}

type Alarm = { time: string; label?: string };

export class Device extends DurableObject<Env> {
  // ---- per-turn state (in memory; the DO stays awake while an xAI socket is open) ----
  private xai: WebSocket | null = null;
  private opening = false;
  private pending: ArrayBuffer[] = [];
  private started = 0;
  private capMs = 0;
  private heardSpeech = false;
  private speaking = false;
  private responseActive = false;
  private toolBusy = false;
  private expectSpeech = true;           // false for relay-initiated turns (greeting, reading a result)
  private closeTimer: ReturnType<typeof setTimeout> | null = null;
  private guard: ReturnType<typeof setInterval> | null = null;
  // Audio pacing: xAI produces speech much faster than real time. Dumping it all on the sphere backs up the socket
  // (its RX drains ~80 KB/s), so control messages (idle, pongs) wait behind seconds of audio and a tap can't stop
  // anything that's already queued. Keep the sphere at most LEAD_MS ahead of real time; the rest waits here.
  private outQ: ArrayBuffer[] = [];
  private outQBytes = 0;
  private paceT0 = 0;
  private paceSent = 0;
  private pump: ReturnType<typeof setInterval> | null = null;
  private idleWhenDrained = false;

  // getWebSockets() order is not guaranteed, and a replaced socket can linger while it closes: send to every live one.
  private dev(): boolean { return this.ctx.getWebSockets("device").some(w => w.readyState === WebSocket.OPEN); }
  private send(data: string | ArrayBuffer) {
    for (const w of this.ctx.getWebSockets("device")) { if (w.readyState === WebSocket.OPEN) { try { w.send(data); } catch {} } }
  }
  private state(s: string) { this.send(JSON.stringify({ state: s })); }
  private queueAudio(buf: ArrayBuffer) {
    for (let off = 0; off < buf.byteLength; off += DEVICE_FRAME_BYTES) { const c = buf.slice(off, off + DEVICE_FRAME_BYTES); this.outQ.push(c); this.outQBytes += c.byteLength; }
    if (!this.paceT0) { this.paceT0 = Date.now(); this.paceSent = 0; }
    if (!this.pump) this.pump = setInterval(() => this.pumpAudio(), 40);
    this.pumpAudio();
  }
  private pumpAudio() {
    const bytesPerMs = OUT_RATE * 2 / 1000;
    const allowed = (Date.now() - this.paceT0 + LEAD_MS) * bytesPerMs;   // real time + lead
    while (this.outQ.length && this.paceSent + this.outQ[0].byteLength <= allowed) {
      const c = this.outQ.shift()!; this.outQBytes -= c.byteLength; this.paceSent += c.byteLength; this.send(c);
    }
    if (!this.outQ.length) {
      if (this.pump) { clearInterval(this.pump); this.pump = null; }
      if (this.idleWhenDrained) { this.idleWhenDrained = false; this.paceT0 = 0; this.state("idle"); }
    }
  }
  private dropAudio() { this.outQ = []; this.outQBytes = 0; this.paceT0 = 0; if (this.pump) { clearInterval(this.pump); this.pump = null; } }
  private day(): string { return new Date().toLocaleDateString("en-CA", { timeZone: this.env.TIMEZONE || "America/Chicago" }); }

  async fetch(req: Request): Promise<Response> {
    const url = new URL(req.url);
    if (url.pathname === "/ws") {
      await this.ctx.storage.put("origin", url.origin);              // where the owner's Grok Bot should post results
      for (const old of this.ctx.getWebSockets("device")) { try { old.close(1000, "replaced"); } catch {} }
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1], ["device"]);
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (url.pathname === "/usage") {
      const m = await this.ctx.storage.list<number>({ prefix: "secs:", reverse: true, limit: 14 });
      const minutes = Object.fromEntries([...m].map(([k, s]) => [k.slice(5), +(s / 60).toFixed(1)]));
      return Response.json({ daily_limit_minutes: Number(this.env.DAILY_MINUTES || "30"), minutes_used: minutes });
    }
    if (url.pathname === "/result") return this.onBotResult(await req.json());
    return new Response("not found", { status: 404 });
  }

  // ---------- device socket (hibernation API) ----------
  async webSocketMessage(_ws: WebSocket, msg: string | ArrayBuffer) {
    if (typeof msg !== "string") {                        // mic audio after a wake
      if (this.xai) { try { this.xai.send(msg); } catch {} return; }
      if (this.outQ.length) { this.dropAudio(); this.idleWhenDrained = false; }   // new wake while a reply drains: drop it
      if (this.pending.length < 100) this.pending.push(msg);
      if (!this.opening) await this.openUpstream(null);
      return;
    }
    let m: any; try { m = JSON.parse(msg); } catch { return; }
    if (m.t === "hello") await this.deliverQueuedResult();
    else if (m.t === "cancel") { if (this.xai) this.closeUpstream("cancelled"); else if (this.outQ.length) { this.dropAudio(); this.idleWhenDrained = false; this.state("idle"); } }   // tap: stop now
    else if (m.t === "alarms" && Array.isArray(m.list)) await this.ctx.storage.put("alarms", m.list.slice(0, 20));
    else if (m.t === "greet" && !this.xai && !this.opening) {
      const label = typeof m.label === "string" && m.label ? ` (the alarm was labelled "${m.label.slice(0, 60)}")` : "";
      await this.openUpstream(`${who(this.env).owner}'s alarm just went off and they turned it off${label}. Say a short, warm good morning with the time, and offer the weather in one sentence. Then stop.`);
    }
  }
  async webSocketClose() { this.closeUpstream("device_closed"); }
  async webSocketError() { this.closeUpstream("device_error"); }

  // ---------- budget ----------
  private async remainingSecs(): Promise<number> {
    const used = (await this.ctx.storage.get<number>("secs:" + this.day())) ?? 0;
    return Math.max(0, Number(this.env.DAILY_MINUTES || "30") * 60 - used);
  }
  private async addSecs(s: number) {
    const k = "secs:" + this.day();
    await this.ctx.storage.put(k, ((await this.ctx.storage.get<number>(k)) ?? 0) + s);
  }

  // ---------- xAI session ----------
  private canDelegate(): boolean { return !!(this.env.GROKBOT_WEBHOOK_URL && this.env.GROKBOT_WEBHOOK_KEY && this.env.BOT_TOKEN); }
  private tools() {
    const t: any[] = [
      { type: "function", name: "get_time", description: "Current local date and time at home.", parameters: { type: "object", properties: {} } },
      { type: "function", name: "get_weather", description: "Current weather and today's high/low at home.", parameters: { type: "object", properties: {} } },
      { type: "function", name: "set_alarm", description: "Set an alarm on the sphere. It rings until someone taps it.",
        parameters: { type: "object", properties: { time: { type: "string", description: "24-hour local time HH:MM, e.g. 06:30" }, label: { type: "string" } }, required: ["time"] } },
      { type: "function", name: "cancel_alarm", description: "Cancel an alarm by time, or all alarms.",
        parameters: { type: "object", properties: { time: { type: "string", description: "HH:MM, or 'all'" } }, required: ["time"] } },
      { type: "function", name: "list_alarms", description: "List the alarms set on the sphere.", parameters: { type: "object", properties: {} } },
      { type: "function", name: "set_volume", description: "Change the sphere's speaker volume (5 levels). Use 'up'/'down' for relative changes, or a level 1-5.",
        parameters: { type: "object", properties: { change: { type: "string", enum: ["up", "down", "set"] }, level: { type: "integer", minimum: 1, maximum: 5 } }, required: ["change"] } },
    ];
    if (this.canDelegate())
      t.push({ type: "function", name: "delegate_to_charlie", description: "Hand a bigger job to the owner's Grok Bot (calendar, email, reminders, research). The answer comes back later.",
        parameters: { type: "object", properties: { task: { type: "string", description: "The complete task in plain words, with any names, dates and details given." } }, required: ["task"] } });
    return t;
  }

  private async openUpstream(injectText: string | null) {
    this.opening = true;
    try {
      const left = await this.remainingSecs();
      if (left < 15) { console.log(JSON.stringify({ ev: "budget_exhausted" })); this.state("idle"); this.pending = []; return; }
      const model = encodeURIComponent(this.env.XAI_MODEL || "grok-voice-3.0");
      const resp = await fetch(`https://api.x.ai/v1/realtime?model=${model}`, { headers: { Upgrade: "websocket", Authorization: `Bearer ${this.env.XAI_API_KEY}` } });
      const ws = resp.webSocket;
      if (!ws) { console.log(JSON.stringify({ ev: "upstream_failed", status: resp.status, body: (await resp.text()).slice(0, 300) })); this.state("idle"); this.pending = []; return; }
      ws.binaryType = "arraybuffer";
      ws.accept();
      this.xai = ws; this.started = Date.now(); this.heardSpeech = false; this.speaking = false;
      this.expectSpeech = injectText === null;
      this.capMs = Math.min(Number(this.env.MAX_SESSION_SECONDS || "120") * 1000, left * 1000);
      console.log(JSON.stringify({ ev: "session_start", left, kind: injectText ? "inject" : "voice" }));
      ws.send(JSON.stringify({ type: "session.update", session: {
        voice: this.env.XAI_VOICE || "eve", instructions: persona(who(this.env), this.canDelegate(), localNow(this.env)),
        tools: this.tools(), reasoning: { effort: this.env.XAI_REASONING || "none" },
        turn_detection: { type: "server_vad" },
        audio: { input: { format: { type: "audio/pcm", rate: IN_RATE }, transport: "binary" },
                 output: { format: { type: "audio/pcm", rate: OUT_RATE }, transport: "binary" } },
      } }));
      if (injectText) {
        ws.send(JSON.stringify({ type: "conversation.item.create", item: { type: "message", role: "user", content: [{ type: "input_text", text: injectText }] } }));
        ws.send(JSON.stringify({ type: "response.create" }));
        this.state("thinking");
      }
      for (const f of this.pending) ws.send(f);
      this.pending = [];
      ws.addEventListener("message", (e: MessageEvent) => { this.onUpstream(e).catch(err => console.log("upstream handler", String(err))); });
      ws.addEventListener("close", () => { if (this.xai === ws) this.closeUpstream("upstream_closed"); });
      ws.addEventListener("error", () => { if (this.xai === ws) this.closeUpstream("upstream_error"); });
      this.guard = setInterval(() => {
        if (!this.xai) return;
        const age = Date.now() - this.started;
        if (this.expectSpeech && !this.heardSpeech && age > Number(this.env.NO_SPEECH_SECONDS || "8") * 1000) this.closeUpstream("no_speech");
        else if (age > this.capMs) this.closeUpstream("session_cap");
      }, 1000);
    } catch (err) {
      console.log(JSON.stringify({ ev: "open_error", err: String(err) })); this.state("idle"); this.pending = [];
    } finally { this.opening = false; }
  }

  private closeUpstream(reason: string) {
    if (!this.xai) return;
    const secs = Math.max(1, Math.ceil((Date.now() - this.started) / 1000));
    try { this.xai.close(1000, reason); } catch {}
    this.xai = null; this.pending = []; this.speaking = this.responseActive = this.toolBusy = false;
    if (this.closeTimer) { clearTimeout(this.closeTimer); this.closeTimer = null; }
    if (this.guard) { clearInterval(this.guard); this.guard = null; }
    console.log(JSON.stringify({ ev: "session_end", reason, secs, queued_ms: Math.round(this.outQBytes / (OUT_RATE * 2 / 1000)) }));
    this.ctx.waitUntil(this.addSecs(secs).then(() => this.deliverQueuedResult()));
    if (reason === "cancelled" || reason.startsWith("device") || !this.outQ.length) { this.dropAudio(); this.state("idle"); }
    else this.idleWhenDrained = true;        // finish sending the reply first; "idle" goes out after the last audio
  }

  private async onUpstream(e: MessageEvent) {
    if (e.data instanceof ArrayBuffer) {
      if (!this.speaking) { this.speaking = true; this.state("speaking"); }
      this.queueAudio(e.data);
      return;
    }
    let ev: any; try { ev = JSON.parse(e.data as string); } catch { return; }
    switch (ev.type) {
      case "input_audio_buffer.speech_started": this.heardSpeech = true; this.speaking = false; this.state("listening"); break;
      case "input_audio_buffer.speech_stopped": this.state("thinking"); break;   // device stops its mic here
      case "response.created": this.responseActive = true; if (this.closeTimer) { clearTimeout(this.closeTimer); this.closeTimer = null; } break;
      case "response.function_call_arguments.done": {
        this.toolBusy = true;
        let args: any = {}; try { args = JSON.parse(ev.arguments || "{}"); } catch {}
        const out = await this.runTool(ev.name, args);
        try {
          this.xai?.send(JSON.stringify({ type: "conversation.item.create", item: { type: "function_call_output", call_id: ev.call_id, output: out } }));
          this.xai?.send(JSON.stringify({ type: "response.create" }));
        } catch {}
        this.toolBusy = false;
        break;
      }
      case "response.done":
        this.responseActive = false; this.speaking = false;
        if (this.closeTimer) clearTimeout(this.closeTimer);
        this.closeTimer = setTimeout(() => { if (!this.responseActive && !this.toolBusy) this.closeUpstream("answered"); }, 1500);
        break;
      case "error": console.log(JSON.stringify({ ev: "xai_error", detail: JSON.stringify(ev).slice(0, 400) })); break;
    }
  }

  // ---------- tools ----------
  private async runTool(name: string, a: any): Promise<string> {
    const env = this.env;
    if (name === "get_time") { this.send(JSON.stringify({ card: "clock" })); return localNow(env); }
    if (name === "get_weather") {
      try {
        const u = `https://api.open-meteo.com/v1/forecast?latitude=${env.HOME_LAT}&longitude=${env.HOME_LON}&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&temperature_unit=fahrenheit&timezone=auto&forecast_days=1`;
        const j: any = await (await fetch(u)).json();
        const f = Math.round(j.current.temperature_2m), code = j.current.weather_code;
        const txt = code === 0 ? "Clear" : code < 4 ? "Partly cloudy" : code < 60 ? "Fog or drizzle" : code < 70 ? "Rain" : code < 80 ? "Snow" : "Storms";
        this.send(JSON.stringify({ wx: txt, f, card: "clock" }));
        return `${txt}, ${f} degrees now; high ${Math.round(j.daily.temperature_2m_max[0])}, low ${Math.round(j.daily.temperature_2m_min[0])}.`;
      } catch { return "Weather is unavailable right now."; }
    }
    if (name === "set_alarm") {
      const t = String(a.time || "").trim();
      if (!/^([01]?\d|2[0-3]):[0-5]\d$/.test(t)) return "Invalid time. Use 24-hour HH:MM.";
      const time = t.padStart(5, "0"), label = String(a.label || "").slice(0, 40);
      this.send(JSON.stringify({ alarm: { op: "set", time, label } }));
      return `Alarm set for ${time}${label ? " (" + label + ")" : ""}.`;
    }
    if (name === "cancel_alarm") {
      const time = String(a.time || "all").trim();
      this.send(JSON.stringify({ alarm: { op: "cancel", time } }));
      return time === "all" ? "All alarms cancelled." : `Alarm at ${time} cancelled.`;
    }
    if (name === "list_alarms") {
      const list = (await this.ctx.storage.get<Alarm[]>("alarms")) ?? [];
      return list.length ? "Alarms: " + list.map(x => x.time + (x.label ? " " + x.label : "")).join(", ") + "." : "No alarms are set.";
    }
    if (name === "set_volume") {
      const op = ["up", "down", "set"].includes(a.change) ? a.change : "down";
      const level = Math.min(5, Math.max(1, Number(a.level) || 3));
      this.send(JSON.stringify({ volume: { op, level } }));
      return op === "set" ? `Volume set to ${level} of 5.` : `Volume turned ${op}.`;
    }
    if (name === "delegate_to_charlie") return this.delegate(String(a.task || "").slice(0, MAX_TEXT));
    return "Unknown tool.";
  }

  private async delegate(task: string): Promise<string> {
    const env = this.env;
    if (!task) return "No task given.";
    const hourKey = "tasks:" + new Date().toISOString().slice(0, 13);
    const n = (await this.ctx.storage.get<number>(hourKey)) ?? 0;
    if (n >= Number(env.TASKS_PER_HOUR || "20")) return "Too many tasks this hour; say to try again later.";
    await this.ctx.storage.put(hourKey, n + 1);
    const request_id = crypto.randomUUID();
    const origin = (await this.ctx.storage.get<string>("origin")) || "";
    await this.ctx.storage.put("task:" + request_id, { task, at: Date.now() });
    try {
      const r = await fetch(env.GROKBOT_WEBHOOK_URL!, {
        method: "POST",
        headers: { "content-type": "application/json", Authorization: `Bearer ${env.GROKBOT_WEBHOOK_KEY}` },
        body: JSON.stringify({ request_id, task, source: "charlie-sphere", requested_at: new Date().toISOString(),
          reply: { url: `${origin}/result`, method: "POST", headers: { Authorization: `Bearer ${env.BOT_TOKEN}`, "content-type": "application/json" },
                   json_shape: { request_id, status: "done | error | attention", spoken_text: "what the assistant should say, one to three sentences" } } }),
      });
      console.log(JSON.stringify({ ev: "task_sent", status: r.status }));
      return r.ok ? "Task handed off. Say you're on it and will get back to them." : "The hand-off failed; say it didn't go through.";
    } catch { return "The hand-off failed; say it didn't go through."; }
  }

  // ---------- Grok Bot results ----------
  private async onBotResult(body: any): Promise<Response> {
    const text = String(body?.spoken_text || body?.text || "").trim().slice(0, MAX_TEXT);
    if (!text) return Response.json({ ok: false, error: "spoken_text required" }, { status: 400 });
    await this.ctx.storage.put("result:queued", { text, status: String(body?.status || "done"), at: Date.now() });
    if (body?.request_id) await this.ctx.storage.delete("task:" + String(body.request_id));
    await this.deliverQueuedResult();
    return Response.json({ ok: true });
  }
  private async deliverQueuedResult() {
    if (this.xai || this.opening || this.outQ.length || !this.dev()) return;          // busy or offline: deliver later
    const q = await this.ctx.storage.get<{ text: string; status: string }>("result:queued");
    if (!q) return;
    await this.ctx.storage.delete("result:queued");
    this.send(JSON.stringify({ chime: "result" }));
    const owner = who(this.env).owner;
    const lead = q.status === "attention" ? `Something needs ${owner}'s attention.` : q.status === "error" ? "The task ran into a problem." : "A task you handed off just finished.";
    await this.openUpstream(`${lead} Read this to ${owner} naturally, in your own words, in one to three sentences, then stop: ${q.text}`);
  }
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const device = () => env.DEVICE.get(env.DEVICE.idFromName("device"));
    if (url.pathname === "/health") return new Response("ok");
    if (url.pathname === "/usage") {
      if (!deviceTokenOk(env, url.searchParams.get("token"))) return new Response("unauthorized", { status: 401 });
      return device().fetch(req);
    }
    if (url.pathname === "/ws") {
      if (req.headers.get("Upgrade") !== "websocket") return new Response("expected websocket", { status: 426 });
      if (!deviceTokenOk(env, url.searchParams.get("token"))) return new Response("unauthorized", { status: 401 });
      if (!env.XAI_API_KEY) return new Response("relay not configured", { status: 503 });
      return device().fetch(req);
    }
    if (url.pathname === "/result" && req.method === "POST") {
      if (!env.BOT_TOKEN || !safeEqual(bearer(req), env.BOT_TOKEN)) return new Response("unauthorized", { status: 401 });
      if (Number(req.headers.get("content-length") || 0) > 16384) return new Response("too large", { status: 413 });
      return device().fetch(req);
    }
    return new Response("not found", { status: 404 });
  },
};
