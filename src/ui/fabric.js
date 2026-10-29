// The fabric: a woven mesh of light behind every page, with the subnet's 13
// nodes in it. Pages call Fabric.think(true) while the chain computes, and
// Fabric.consensus() when its answer lands. Raw WebGL, no libraries.
//
// It pauses when the tab is hidden, renders at reduced resolution, shows a
// still frame for prefers-reduced-motion, and does nothing without WebGL.

const VERT = `attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }`;

const FRAG = `
precision mediump float;
uniform vec2 res;
uniform float t;
uniform vec2 mouse;    // 0..1, y up
uniform float think;   // 0..1, eased
uniform float pulse;   // seconds since the last consensus, large when none
uniform float scroll;  // pixels
uniform vec3 ring;     // centre (uv units) and radius
uniform float ringOn;  // 1: draw the ring in the fabric

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 4; i++) { v += a * noise(p); p *= 2.03; a *= 0.5; }
  return v;
}
// The subnet: 13 machines on a ring (ringXY, ringR), drawn into the fabric
// on the home page (ringOn). Other pages draw it in a corner badge instead,
// and the consensus ripple starts from there.
vec2 nodeAt(float i, vec3 r) {
  float a = 1.5708 - i * 0.48332; // 2pi / 13, clockwise from the top
  return r.xy + vec2(cos(a), sin(a)) * r.z;
}
float seg(vec2 p, vec2 a, vec2 b) {
  vec2 ab = b - a;
  return distance(p, a + ab * clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0));
}

void main() {
  float aspect = res.x / res.y;
  vec2 uv = gl_FragCoord.xy / res.y;          // y 0..1, x 0..aspect
  vec2 m = vec2(mouse.x * aspect, mouse.y);
  float slow = t * 0.035;

  // Cloth: warp the plane with slow noise, a swell around the pointer, and
  // the consensus ripple, then weave two families of threads through it.
  vec2 q = uv + vec2(0.0, scroll * 0.00012);
  vec2 warp = vec2(fbm(q * 1.6 + slow), fbm(q * 1.6 - slow + 7.3)) - 0.5;
  float dm = distance(uv, m);
  vec2 push = (uv - m) * exp(-dm * dm * 18.0) * 0.06;
  float reach = pulse * 0.55;
  vec3 ring3 = ring;
  vec2 centre = ring3.xy;
  float wave = exp(-pow((distance(uv, centre) - reach) * 7.0, 2.0)) * exp(-pulse * 0.7);
  vec2 w = q + warp * (0.32 + think * 0.12) + push + normalize(uv - centre + 1e-4) * wave * 0.07;

  float dens = 21.0;
  vec2 g = w * dens;
  vec2 fw = fwidth(g) + 0.0001;
  vec2 line = abs(fract(g) - 0.5) / fw;
  float warpT = 1.0 - min(line.x, 1.0);
  float weftT = 1.0 - min(line.y, 1.0);
  // Over-under weave: threads brighten where they pass on top.
  float over = step(0.5, fract(floor(g.x) * 0.5 + floor(g.y) * 0.5));
  float threads = max(warpT * (0.55 + 0.45 * over), weftT * (0.55 + 0.45 * (1.0 - over)));

  float shade = fbm(w * 3.0 + slow * 2.0);
  // Silk: glossy bands drift across the folds of the cloth.
  float fold = fbm(q * 1.1 + slow * 0.7);
  float sheen = pow(0.5 + 0.5 * sin((w.x * 0.8 + w.y * 1.3) * 3.2 + fold * 7.0 - t * 0.12), 8.0);
  vec3 navy = mix(vec3(0.018, 0.035, 0.08), vec3(0.03, 0.06, 0.11), uv.y);
  vec3 mint = vec3(0.37, 0.95, 0.78);
  float glow = 0.045 + 0.08 * shade + sheen * 0.22 + think * 0.09 + wave * 1.4 + exp(-dm * 6.0) * 0.05;
  vec3 col = navy + mint * threads * glow;
  col += vec3(0.25, 0.55, 0.6) * sheen * 0.035 + mint * wave * 0.05;

  // The subnet. At rest the 13 blink together once a second, like blocks
  // being finalized. While the chain computes, packets run along the links
  // and votes fill around the ring; on consensus everything flashes at once.
  float beat = exp(-fract(t) * 5.0);
  float agree = exp(-pulse * 1.6);
  float votes = fract(t * 0.55) * 13.0;
  float net = 0.0, pkt = 0.0, core = 0.0, halo = 0.0;
  if (ringOn > 0.5) for (int i = 0; i < 13; i++) {
    float fi = float(i);
    vec2 a = nodeAt(fi, ring3);
    for (int k = 0; k < 2; k++) {
      float hop = k == 0 ? 1.0 : 5.0;
      vec2 b = nodeAt(mod(fi + hop, 13.0), ring3);
      net += smoothstep(0.0022, 0.0, seg(uv, a, b)) * (k == 0 ? 1.0 : 0.6);
      float h = fract(sin(fi * 12.9 + hop * 7.7) * 437.5);
      float f = fract(t * (0.45 + 0.35 * h) + h);
      vec2 dd = uv - mix(a, b, f);
      pkt += exp(-dot(dd, dd) * 26000.0) * sin(f * 3.14159);
    }
    float d = distance(uv, a);
    float lit = 0.3 + 0.25 * beat + think * 0.9 * step(fi, votes) + 1.4 * agree;
    core += smoothstep(0.0075, 0.004, d) * lit;
    core += smoothstep(0.0025, 0.0, abs(d - 0.013)) * 0.7 * lit;
    halo += 0.00004 / (d * d + 0.00012) * lit;
  }
  col += mint * net * (0.11 + 0.06 * beat + 0.16 * think + 0.5 * agree);
  col += mint * pkt * think * 0.9;
  col += mint * min(core, 2.0) * 0.55 + mint * halo * 0.6;

  // Vignette, so text stays easy to read.
  vec2 c = gl_FragCoord.xy / res - 0.5;
  col *= 1.0 - dot(c, c) * 0.9;
  gl_FragColor = vec4(col, 1.0);
}`;

const state = { think: 0, target: 0, lastPulse: -1e3 };
const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;

function start() {
  const canvas = document.createElement("canvas");
  canvas.setAttribute("aria-hidden", "true");
  Object.assign(canvas.style, { position: "fixed", inset: "0", width: "100vw", height: "100vh", zIndex: "-1", pointerEvents: "none" });
  const gl = canvas.getContext("webgl", { antialias: false, alpha: false, powerPreference: "low-power" });
  if (!gl || !gl.getExtension("OES_standard_derivatives")) return null; // stay with the page's own background
  document.body.prepend(canvas);

  const compile = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, type === gl.FRAGMENT_SHADER ? "#extension GL_OES_standard_derivatives : enable\n" + src : src);
    gl.compileShader(s);
    return gl.getShaderParameter(s, gl.COMPILE_STATUS) ? s : null;
  };
  const vs = compile(gl.VERTEX_SHADER, VERT), fs = compile(gl.FRAGMENT_SHADER, FRAG);
  if (!vs || !fs) { canvas.remove(); return null; }
  const prog = gl.createProgram();
  gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) { canvas.remove(); return null; }
  gl.useProgram(prog);
  gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
  const loc = gl.getAttribLocation(prog, "p");
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
  const u = (n) => gl.getUniformLocation(prog, n);
  const U = { res: u("res"), t: u("t"), mouse: u("mouse"), think: u("think"), pulse: u("pulse"), scroll: u("scroll"), ring: u("ring"), ringOn: u("ringOn") };

  // Reduced resolution: the fabric is soft, and phones stay cool.
  const scale = Math.min(1, (window.devicePixelRatio || 1) * (innerWidth < 760 ? 0.45 : 0.6));
  const resize = () => {
    canvas.width = Math.max(1, Math.floor(innerWidth * scale));
    canvas.height = Math.max(1, Math.floor(innerHeight * scale));
    gl.viewport(0, 0, canvas.width, canvas.height);
  };
  resize();
  addEventListener("resize", resize);

  const mouse = [0.5, 0.5], eased = [0.5, 0.5];
  addEventListener("pointermove", (e) => { mouse[0] = e.clientX / innerWidth; mouse[1] = 1 - e.clientY / innerHeight; }, { passive: true });

  const t0 = performance.now();
  let raf = 0;
  const draw = (now) => {
    const t = (now - t0) / 1000;
    state.think += (state.target - state.think) * 0.05;
    eased[0] += (mouse[0] - eased[0]) * 0.06; eased[1] += (mouse[1] - eased[1]) * 0.06;
    gl.uniform2f(U.res, canvas.width, canvas.height);
    gl.uniform1f(U.t, reduce ? 20 : t);
    gl.uniform2f(U.mouse, eased[0], eased[1]);
    gl.uniform1f(U.think, state.think);
    gl.uniform1f(U.pulse, reduce ? 1e3 : t - state.lastPulse);
    gl.uniform1f(U.scroll, scrollY);
    const r = ringNow();
    gl.uniform3f(U.ring, r[0], r[1], r[2]);
    gl.uniform1f(U.ringOn, hero ? 1 : 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    if (!hero) drawBadge(reduce ? 20 : t, state.think, Math.exp(-(t - state.lastPulse) * 1.6));
  };
  const loop = (now) => { draw(now); raf = requestAnimationFrame(loop); };
  const run = () => { cancelAnimationFrame(raf); if (reduce) draw(performance.now()); else raf = requestAnimationFrame(loop); };
  document.addEventListener("visibilitychange", () => (document.hidden ? cancelAnimationFrame(raf) : run()));
  if (reduce) addEventListener("resize", () => draw(performance.now()));
  run();
  hud(canvas);
  return { now: () => (performance.now() - t0) / 1000, redraw: () => reduce && draw(performance.now()) };
}

// Where the ring is, in the shader's units: x and y in viewport heights, y
// up. On the home page it is big, upper right (upper centre on narrow
// screens); elsewhere it is the badge in the bottom-right corner.
const hero = document.body.dataset.fabric === "hero";
let badge = null, bctx = null;
function ringNow() {
  const aspect = innerWidth / innerHeight;
  if (hero) return aspect < 1 ? [0.5 * aspect, 0.8, 0.12] : [0.8 * aspect, 0.7, 0.17];
  const b = badge.getBoundingClientRect();
  return [(b.left + b.width / 2) / innerHeight, 1 - (b.top + b.height / 2) / innerHeight, 0.05];
}

// The label: in the middle of the big ring, or inside the badge.
let label = null, labelTimer = 0;
const LABEL = { rest: "1 subnet<br>13 nodes", think: "13 nodes<br>computing", agree: "\u2713 13 of 13<br>agreed" };
const BADGE = 150; // css px
function hud(canvas) {
  label = document.createElement("div");
  label.className = "fabric-hud";
  label.setAttribute("aria-hidden", "true");
  if (hero) {
    canvas.after(label);
    const place = () => {
      const [x, y] = ringNow();
      label.style.left = x * innerHeight + "px";
      label.style.top = (1 - y) * innerHeight + "px";
    };
    place();
    addEventListener("resize", place);
  } else {
    badge = document.createElement("div");
    badge.className = "fabric-badge";
    badge.setAttribute("aria-hidden", "true");
    const c = document.createElement("canvas");
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    c.width = c.height = BADGE * dpr;
    bctx = c.getContext("2d");
    bctx.scale(dpr, dpr);
    badge.append(c, label);
    document.body.append(badge);
  }
  setLabel("rest");
}

// The badge's ring, the same story as the shader's: a shared blink once a
// second at rest, packets and votes while computing, a flash on consensus.
function drawBadge(t, think, agree) {
  const g = bctx, m = BADGE / 2, R = 52;
  const at = (i) => { const a = -Math.PI / 2 + (i % 13) * (2 * Math.PI / 13); return [m + Math.cos(a) * R, m + Math.sin(a) * R]; };
  const beat = Math.exp(-(t % 1) * 5), votes = ((t * 0.55) % 1) * 13;
  const mint = (a) => `rgba(128,255,229,${Math.min(1, a).toFixed(3)})`;
  g.clearRect(0, 0, BADGE, BADGE);
  g.lineWidth = 1;
  for (let i = 0; i < 13; i++) {
    const a = at(i);
    for (const hop of [1, 5]) {
      const b = at(i + hop);
      g.strokeStyle = mint((0.13 + 0.08 * beat + 0.3 * think + 0.7 * agree) * (hop === 1 ? 1 : 0.6));
      g.beginPath(); g.moveTo(a[0], a[1]); g.lineTo(b[0], b[1]); g.stroke();
      if (think > 0.02) {
        const h = (Math.sin(i * 12.9 + hop * 7.7) * 437.5) % 1, f = (t * (0.45 + 0.35 * Math.abs(h)) + Math.abs(h)) % 1;
        g.fillStyle = mint(think * Math.sin(f * Math.PI));
        g.beginPath(); g.arc(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, 1.8, 0, 7); g.fill();
      }
    }
  }
  for (let i = 0; i < 13; i++) {
    const [x, y] = at(i);
    const lit = 0.3 + 0.25 * beat + think * 0.9 * (i <= votes ? 1 : 0) + 1.4 * agree;
    g.fillStyle = mint(0.25 + lit * 0.6);
    g.shadowColor = mint(lit * 0.8); g.shadowBlur = 8 * Math.min(lit, 1.5);
    g.beginPath(); g.arc(x, y, 3.4, 0, 7); g.fill();
    g.shadowBlur = 0;
    g.strokeStyle = mint(lit * 0.45);
    g.beginPath(); g.arc(x, y, 6.5, 0, 7); g.stroke();
  }
}
function setLabel(mode) {
  if (!label) return;
  clearTimeout(labelTimer);
  label.innerHTML = LABEL[mode];
  label.dataset.mode = mode;
  if (badge) badge.dataset.mode = mode;
  if (mode === "agree") labelTimer = setTimeout(() => setLabel("rest"), 3200);
}

const fabric = document.readyState === "loading" ? null : start();
let live = fabric;
if (!live) document.addEventListener("DOMContentLoaded", () => { live = start(); });

window.Fabric = {
  // The chain is computing (true) or done (false).
  think(on) { state.target = on ? 1 : 0; setLabel(on ? "think" : "rest"); live && live.redraw(); },
  // The chain's answer landed: a ripple runs across the fabric.
  consensus() { state.target = 0; setLabel("agree"); if (live) { state.lastPulse = live.now(); live.redraw(); } },
};
