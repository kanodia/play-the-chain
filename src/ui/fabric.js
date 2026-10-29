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
// The node positions: an arc across the upper right of the page.
vec2 nodeAt(float i, float aspect) {
  float a = 3.65 + i * 0.165;
  return vec2(0.78 * aspect + cos(a) * 0.55, 0.88 + sin(a) * 0.32);
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
  float ring = pulse * 0.55;
  vec2 centre = vec2(0.78 * aspect, 0.62);
  float wave = exp(-pow((distance(uv, centre) - ring) * 7.0, 2.0)) * exp(-pulse * 0.7);
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

  // The 13 nodes: dim at rest; while thinking they light one by one.
  for (int i = 0; i < 13; i++) {
    float fi = float(i);
    vec2 n = nodeAt(fi, aspect) + warp * 0.05;
    float d = distance(uv, n);
    float on = think * smoothstep(0.0, 0.4, fract(t * 0.9 - fi / 13.0)) + 0.25 + 1.2 * exp(-pulse * 2.5);
    col += mint * (0.0022 / (d * d + 0.0009)) * 0.012 * on;
    col += mint * smoothstep(0.012, 0.0, d) * 0.5 * min(on, 1.2);
  }

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
  const U = { res: u("res"), t: u("t"), mouse: u("mouse"), think: u("think"), pulse: u("pulse"), scroll: u("scroll") };

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
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  };
  const loop = (now) => { draw(now); raf = requestAnimationFrame(loop); };
  const run = () => { cancelAnimationFrame(raf); if (reduce) draw(performance.now()); else raf = requestAnimationFrame(loop); };
  document.addEventListener("visibilitychange", () => (document.hidden ? cancelAnimationFrame(raf) : run()));
  if (reduce) addEventListener("resize", () => draw(performance.now()));
  run();
  return { now: () => (performance.now() - t0) / 1000, redraw: () => reduce && draw(performance.now()) };
}

const fabric = document.readyState === "loading" ? null : start();
let live = fabric;
if (!live) document.addEventListener("DOMContentLoaded", () => { live = start(); });

window.Fabric = {
  // The chain is computing (true) or done (false).
  think(on) { state.target = on ? 1 : 0; live && live.redraw(); },
  // The chain's answer landed: a ripple runs across the fabric.
  consensus() { state.target = 0; if (live) { state.lastPulse = live.now(); live.redraw(); } },
};
