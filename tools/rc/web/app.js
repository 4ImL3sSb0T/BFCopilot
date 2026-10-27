"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const text = (selector, value) => { $(selector).textContent = value; };
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const finite = value => typeof value === "number" && Number.isFinite(value);
const fmt = (value, digits = 0) => finite(value) ? value.toFixed(digits) : "—";
const signed = value => finite(value) ? `${value >= 0 ? "+" : ""}${value.toFixed(2)}` : "—";
const escapeHTML = value => String(value).replace(/[&<>"']/g, character => ({
  "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
})[character]);
const icons = {
  dashboard: '<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
  controller: '<path d="M7 7h10c2 0 3 2 4 8l.5 3c.4 3-2.2 3-4 1l-2-2h-7l-2 2c-2 2-4.5 2-4-1L3 15c1-6 2-8 4-8Z"/><path d="M6 11v4m-2-2h4m7-1h.01m3 2h.01"/>',
  sliders: '<path d="M5 3v5m0 5v8M12 3v9m0 5v4M19 3v3m0 5v10"/><rect x="3" y="8" width="4" height="5" rx="1"/><rect x="10" y="12" width="4" height="5" rx="1"/><rect x="17" y="6" width="4" height="5" rx="1"/>',
  list: '<path d="M8 6h12M8 12h12M8 18h12M3 6h.01M3 12h.01M3 18h.01"/>',
  scan: '<path d="M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5M8 12a4 4 0 0 1 4-4m4 4a4 4 0 0 1-4 4"/><circle cx="12" cy="12" r="1"/>',
  arrow: '<path d="M4 12h15m-5-5 5 5-5 5"/>',
  lock: '<rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3m-4 5v2"/>',
  unlock: '<rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 7-2m-3 10v2"/>',
  shield: '<path d="m12 2 8 3v6c0 6-8 11-8 11S4 17 4 11V5Z"/><path d="m8 11 3 3 5-6"/>',
  reset: '<path d="M3 10a9 9 0 1 1 2 8m-2-8V4m0 6h6"/>',
  stop: '<rect x="5" y="5" width="14" height="14" rx="3"/>',
  close: '<path d="m6 6 12 12M6 18 18 6"/>',
  eye: '<path d="M2 12S6 5 12 5s10 7 10 7-4 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/>',
};
const icon = name => `<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">${icons[name] || icons.dashboard}</svg>`;
function hydrateIcons(root = document) {
  $$('[data-icon]', root).forEach(element => { element.outerHTML = icon(element.dataset.icon); });
}

const token = location.hash.slice(1);
const client = crypto.randomUUID();
let state = null;
let config = null;
let initialized = false;
let chartType = "sticks";
let drawerType = "";
let selectedButton = null;
let lastLogId = "";
let lastHistoryAt = 0;
let pollFailed = false;
let toastTimeout;
let holding = false;
let holdStarted = 0;
let holdTimer;
let holdPending = false;
const history = [];
const pendingChanges = new Map();
const changeTimers = new Map();
const headers = { "X-BF-Token": token, "X-BF-Client": client, "Content-Type": "application/json" };

function toast(message, error = false) {
  const element = $("#toast");
  element.textContent = message;
  element.classList.toggle("error", error);
  element.hidden = false;
  clearTimeout(toastTimeout);
  toastTimeout = setTimeout(() => { element.hidden = true; }, 4000);
}

async function action(name, values = {}, quiet = false) {
  try {
    const response = await fetch("/api/action", {
      method: "POST", headers, body: JSON.stringify({ action: name, ...values }),
      signal: AbortSignal.timeout(1800),
    });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || "操作未完成");
    return true;
  } catch (error) {
    if (!quiet) toast(error.message, true);
    return false;
  }
}

function scheduleChange(group, key, value) {
  if (!pendingChanges.has(group)) pendingChanges.set(group, {});
  pendingChanges.get(group)[key] = value;
  clearTimeout(changeTimers.get(group));
  changeTimers.set(group, setTimeout(async () => {
    const values = pendingChanges.get(group);
    pendingChanges.delete(group);
    const ok = await action("configure", { group, values });
    if (ok && config) Object.assign(config[group], values);
  }, 80));
}

async function flushChanges() {
  for (const [group, values] of pendingChanges) {
    clearTimeout(changeTimers.get(group));
    pendingChanges.delete(group);
    if (!await action("configure", { group, values })) return false;
  }
  return true;
}

function controllerSVG() {
  const screw = (x, y) => `<g><circle cx="${x}" cy="${y}" r="3.4" fill="#c5d0d8"/><circle cx="${x}" cy="${y}" r="2.3" fill="#e8edf1"/><path d="M${x - 1.2} ${y}h2.4" stroke="#9eafba" stroke-width=".7"/></g>`;
  const gimbal = (cx, side) => {
    const ticks = Array.from({ length: 36 }, (_, index) => {
      const angle = index * Math.PI / 18;
      const outer = 51, inner = index % 3 ? 48.5 : 46;
      return `<path d="M${cx + Math.cos(angle) * outer} ${170 + Math.sin(angle) * outer}L${cx + Math.cos(angle) * inner} ${170 + Math.sin(angle) * inner}" stroke="#a5b5c0" stroke-width=".8"/>`;
    }).join("");
    return `<g><circle cx="${cx}" cy="173" r="57" fill="#bbc9d3"/><circle cx="${cx}" cy="170" r="57" fill="#d8e2e9" stroke="#c2ced8"/><circle cx="${cx}" cy="170" r="53" fill="#e8edf2"/>${ticks}<circle cx="${cx}" cy="170" r="43" fill="#526675"/><circle cx="${cx}" cy="171" r="39" fill="url(#gimbal)" stroke="#304b5d"/><path d="M${cx - 34} 170h68M${cx} 136v68" stroke="#9eb4c1" stroke-opacity=".22" stroke-dasharray="2 4"/><circle cx="${cx}" cy="170" r="26" fill="none" stroke="#8da5b4" stroke-opacity=".15"/>
      <line id="stem-${side}" x1="${cx}" y1="170" x2="${cx}" y2="165" stroke="#98acb9" stroke-width="9" stroke-linecap="round"/>
      <g id="stick-${side}" transform="translate(${cx} 165)"><ellipse cy="5" rx="16" ry="12" fill="#1a3445" opacity=".3"/><circle r="14" fill="#507e8b" stroke="#294d5c"/><circle cy="-2" r="12" fill="url(#thumb)" stroke="#70a7af"/><circle cy="-2" r="8.5" fill="none" stroke="#b7d5d7" stroke-opacity=".35"/>${Array.from({ length: 5 }, (_, index) => `<path d="M-6 ${index * 2 - 6}h12" stroke="#315b68" stroke-width=".65" opacity=".6"/>`).join("")}</g>
      ${screw(cx - 41, 127)}${screw(cx + 41, 213)}
    </g>`;
  };
  const switches = [203, 266, 414, 477].map((cx, index) => `<g class="map-target" data-switch="${index + 4}" role="button" tabindex="0" aria-label="辅助通道 A${index + 4}，查看映射"><rect x="${cx - 16}" y="75" width="32" height="18" rx="6" fill="#b5c4d0"/><ellipse cx="${cx}" cy="80" rx="10" ry="5" fill="#e6edf2" stroke="#b2c2ce"/><g id="switch-${index + 4}"><path d="M${cx} 79v-14" stroke="#91a5b4" stroke-width="4" stroke-linecap="round"/><rect x="${cx - 4}" y="55" width="8" height="13" rx="2.5" fill="#506878"/><path d="M${cx - 2} 58v7" stroke="#9db1bc" stroke-width=".7"/></g><path class="callout-line" d="M${cx} 51V38H${cx < 340 ? cx - 37 : cx + 37}"/><circle cx="${cx}" cy="51" r="1.5" fill="#8ba4b4"/><text class="control-label" x="${cx < 340 ? cx - 39 : cx + 39}" y="33" text-anchor="${cx < 340 ? 'end' : 'start'}">辅助 ${index + 1}</text><text id="aux-${index + 4}" class="control-small" x="${cx < 340 ? cx - 39 : cx + 39}" y="46" text-anchor="${cx < 340 ? 'end' : 'start'}">A${index + 4} · —</text></g>`).join("");
  return `<svg viewBox="40 20 600 285" role="img" aria-label="LiteRadio 风格遥控器，左摇杆油门与偏航，右摇杆俯仰与横滚，顶部四路辅助通道"><defs>
    <linearGradient id="shell" x1="0" y1="0" x2="0.2" y2="1"><stop stop-color="#fcfdfd"/><stop offset=".5" stop-color="#f0f3f6"/><stop offset="1" stop-color="#cbd7e0"/></linearGradient>
    <radialGradient id="gimbal" cx=".45" cy=".35"><stop stop-color="#466071"/><stop offset=".75" stop-color="#324a5d"/><stop offset="1" stop-color="#263f52"/></radialGradient>
    <linearGradient id="thumb" x2="0.3" y2="1"><stop stop-color="#8dbfc3"/><stop offset="1" stop-color="#4a8290"/></linearGradient>
    <filter id="controller-shadow" x="-30%" y="-30%" width="160%" height="180%"><feDropShadow dx="0" dy="12" stdDeviation="9" flood-color="#617e92" flood-opacity=".2"/></filter>
  </defs><ellipse cx="340" cy="281" rx="175" ry="15" fill="#bdcdd8" opacity=".16"/>
  <g filter="url(#controller-shadow)"><path d="M190 82C179 82 163 102 159 123L140 231c-7 39 15 65 39 60l67-37c28-15 49-18 94-18s66 3 94 18l67 37c24 5 46-21 39-60l-19-108c-4-21-20-41-31-41H190Z" fill="#b4c5d2"/>
    <path d="M190 77C179 77 163 97 159 118L140 226c-7 39 15 65 39 60l67-37c28-15 49-18 94-18s66 3 94 18l67 37c24 5 46-21 39-60l-19-108c-4-21-20-41-31-41H190Z" fill="url(#shell)" stroke="#bbcbd6" stroke-width="1.1"/>
    <path d="M179 108c-8 13-12 27-15 42l-13 73c-4 21 0 39 14 45l41-20c-32-10-39-47-38-73 0-24 4-45 11-67Z" fill="#d9e3ea"/>
    <path d="M501 108c8 13 12 27 15 42l13 73c4 21 0 39-14 45l-41-20c32-10 39-47 38-73 0-24-4-45-11-67Z" fill="#cddbe5"/>
    ${Array.from({ length: 8 }, (_, index) => `<path d="M${155 - index * .2} ${206 + index * 5}l21-10M${525 + index * .2} ${206 + index * 5}l-21-10" stroke="#b8cbd7" stroke-width="1" stroke-linecap="round" opacity="${.5 - index * .035}"/>`).join("")}
    <path d="M203 91h274" stroke="#fff" stroke-width="1.4"/><path d="M289 106q51-12 102 0l-11 32h-80Z" fill="#e2eaf0" stroke="#d1dce4"/>
    <rect x="323" y="113" width="34" height="7" rx="3.5" fill="#becdd7"/><rect id="usb-led" x="332" y="115" width="16" height="3" rx="1.5" fill="#a2b6c2"/>
    ${gimbal(238, 'left')}${gimbal(442, 'right')}
    <path d="m324 155 16 24 16-24-16 10Z" fill="#8ea5b5"/><path d="M340 165v-15" stroke="#8ea5b5" stroke-width="1.2"/>
    <circle cx="340" cy="199" r="11" fill="#d2dfe8" stroke="#bfceda"/><circle cx="340" cy="197" r="10" fill="#e9eff4" stroke="#cad8e2"/><path d="M336 192a6 6 0 1 0 8 0m-4-3v7" fill="none" stroke="#91a7b7" stroke-width="1.4" stroke-linecap="round"/>
    <text x="340" y="216" text-anchor="middle" fill="#849eaf" font-size="7">实体电源</text>
    <text x="340" y="225" text-anchor="middle" fill="#99aebc" font-size="8" letter-spacing="1">USB CONTROL</text>
    ${screw(186, 103)}${screw(494, 103)}${screw(164, 251)}${screw(516, 251)}
  </g>${switches}
  <g class="map-target" data-map="throttle" role="button" tabindex="0" aria-label="油门，查看轴映射"><path class="callout-line" d="M193 148h-60l-15-17H72"/><circle cx="193" cy="148" r="2" fill="#8ba4b4"/><text class="control-label" x="70" y="119">油门</text><text class="control-small" id="label-throttle" x="70" y="131">A2 · 频率</text></g>
  <g class="map-target" data-map="yaw" role="button" tabindex="0" aria-label="偏航，查看轴映射"><path class="callout-line" d="M189 183h-54l-15 17H72"/><circle cx="189" cy="183" r="2" fill="#8ba4b4"/><text class="control-label" x="70" y="214">偏航</text><text class="control-small" id="label-yaw" x="70" y="226">A0 · 相位</text></g>
  <g class="map-target" data-map="pitch" role="button" tabindex="0" aria-label="俯仰，查看轴映射"><path class="callout-line" d="M487 148h60l15-17h46"/><circle cx="487" cy="148" r="2" fill="#8ba4b4"/><text class="control-label" x="610" y="119" text-anchor="end">俯仰</text><text class="control-small" id="label-pitch" x="610" y="131" text-anchor="end">A1 · 中心角</text></g>
  <g class="map-target" data-map="roll" role="button" tabindex="0" aria-label="横滚，查看轴映射"><path class="callout-line" d="M491 183h54l15 17h48"/><circle cx="491" cy="183" r="2" fill="#8ba4b4"/><text class="control-label" x="610" y="214" text-anchor="end">横滚</text><text class="control-small" id="label-roll" x="610" y="226" text-anchor="end">A3 · 差动</text></g>
  </svg>`;
}

function servoSVG() {
  const servo = (cx, side) => `<g><path d="M${cx - 32} 73v29l9 8h46l9-8V73" fill="#c5d5e0" stroke="#b0c5d2"/><rect x="${cx - 32}" y="66" width="64" height="33" rx="7" fill="#dfe9ef" stroke="#c2d2de"/><rect x="${cx - 38}" y="76" width="10" height="14" rx="2" fill="#c1d2de"/><rect x="${cx + 28}" y="76" width="10" height="14" rx="2" fill="#c1d2de"/><circle cx="${cx - 33}" cy="83" r="2" fill="#f4f8fa"/><circle cx="${cx + 33}" cy="83" r="2" fill="#f4f8fa"/><circle cx="${cx}" cy="80" r="14" fill="#b0c7d4"/><circle cx="${cx}" cy="80" r="10" fill="#ebf1f5" stroke="#a9c0ce"/><path d="M${cx - 47} 74a48 48 0 0 1 94 0" fill="none" stroke="#e1eaf0" stroke-width="4"/><path d="M${cx - 47} 74a48 48 0 0 1 94 0" fill="none" stroke="#9cbcc8" stroke-width="1" stroke-dasharray="1 9"/><g id="servo-arm-${side}" transform="rotate(0 ${cx} 80)"><path d="M${cx - 5} 78l1-42a4 4 0 0 1 8 0l1 42Z" fill="${side === 'left' ? '#6eabb1' : '#91b4c9'}" stroke="${side === 'left' ? '#4a8c98' : '#729aaf'}"/><circle cx="${cx}" cy="39" r="1.8" fill="#e9f5f5"/><circle cx="${cx}" cy="48" r="1.5" fill="#e9f5f5"/><circle cx="${cx}" cy="57" r="1.5" fill="#e9f5f5"/><circle cx="${cx}" cy="80" r="6" fill="#fafcfd" stroke="#a1bccb"/><path d="M${cx - 2} 80h4" stroke="#8aa5b5"/></g><text x="${cx}" y="126" font-size="9" text-anchor="middle" fill="#91a7b6">${side === 'left' ? '左侧' : '右侧'}</text></g>`;
  return `<svg viewBox="0 0 360 145" role="img" aria-label="左右舵机输出指令示意"><path d="M45 112h270" stroke="#e9eff3"/><path d="M145 81h70" stroke="#dce7ee" stroke-dasharray="2 4"/><circle cx="180" cy="81" r="14" fill="#f6f9fb" stroke="#e0e9ef"/><path id="phase-pointer" d="M180 81v-9" stroke="#9cb9c7" stroke-width="2" stroke-linecap="round"/>${servo(91, 'left')}${servo(269, 'right')}</svg>`;
}

hydrateIcons();
$("#controller-figure").innerHTML = controllerSVG();
$("#servo-figure").innerHTML = servoSVG();
const channelNames = { throttle: "油门", yaw: "偏航", pitch: "俯仰", roll: "横滚" };
$("#channel-bars").innerHTML = Object.entries(channelNames).map(([key, label]) => `<div class="channel-row"><span class="channel-label">${label}<small id="axis-${key}">—</small></span><div class="channel-track"><i class="channel-fill" id="bar-${key}"></i></div><b class="channel-value" id="value-${key}">—</b></div>`).join("");

function badge(selector, label, kind = "neutral", dot = false) {
  const element = $(selector);
  element.className = `badge ${kind}`;
  element.replaceChildren();
  if (dot) element.append(document.createElement("i"));
  element.append(document.createTextNode(label));
}

function setUnit(selector, value, unit) {
  const element = $(selector);
  element.replaceChildren(document.createTextNode(value));
  const small = document.createElement("small");
  small.textContent = unit;
  element.append(small);
}

function render(data) {
  const snapshot = data.snapshot;
  const statusTimeout = (snapshot?.status_timeout_ms ?? 150) / 1000;
  const connected = data.bridge_ok && data.active && snapshot?.state === "connected" && snapshot.status_age < statusTimeout;
  const status = connected ? snapshot.status : null;
  const armed = Boolean(status?.armed);
  const sticks = data.sticks;
  const present = Boolean(sticks?.connected && data.bridge_ok);
  $("#demo-banner").hidden = !data.demo;
  if (data.observer) {
    text("#notice", "此页面为观察窗口；另一个页面正在控制设备。");
    $("#notice").hidden = false;
  } else if (!data.bridge_ok) {
    text("#notice", "采集线程已停止，请重新启动上位机。");
    $("#notice").classList.add("danger");
    $("#notice").hidden = false;
  } else if (!pollFailed && connected && snapshot.control_ttl_ms === 300) {
    text("#notice", "台架 Wi-Fi · 控制失联保护 300 ms，遥测超时 500 ms；仅限有人看护的台架测试。");
    $("#notice").classList.remove("danger");
    $("#notice").hidden = false;
  } else if (!pollFailed) $("#notice").hidden = true;
  badge("#link-badge", connected ? (data.demo ? "演示链路" : "飞控已连接") : snapshot?.state === "hello" ? "正在连接" : "飞控未连接", connected ? "" : "neutral", true);
  text("#connect", data.active && !data.demo ? "断开连接" : "连接飞控");
  badge("#joystick-badge", present ? (data.demo ? "模拟输入" : "USB 已连接") : "等待 USB 摇杆", present ? "" : "neutral", true);
  text("#device-model", data.device?.name ? data.device.name.replace(" · 演示", "") : "LiteRadio 3 / USB HID");
  text("#device-specs", present ? `${data.device?.axes ?? 0} 轴 / ${data.device?.buttons ?? 0} 键` : "—");
  text("#input-caption", present ? (data.demo ? "模拟拨杆与按键 · 演示数据" : "USB 输入正常 · 点击标注调整映射") : (data.device?.error || "插入 USB 摇杆，拨动即可观察；无需连接飞控"));
  $("#input-dot").classList.toggle("off", !present);
  $("#usb-led").setAttribute("fill", present ? "#68c2ba" : "#a2b6c2");
  for (const [key] of Object.entries(channelNames)) {
    const value = present ? sticks[key] : null;
    const valueText = key === "throttle" ? `${fmt(value === null ? null : value * 100)}%` : signed(value);
    text(`#read-${key}`, valueText);
    text(`#value-${key}`, valueText);
    text(`#axis-${key}`, `A${data.config.roles[key]}`);
    const percent = value === null ? 0 : key === "throttle" ? clamp(value, 0, 1) * 100 : Math.abs(clamp(value, -1, 1)) * 50;
    const start = key === "throttle" ? 0 : finite(value) && value < 0 ? 50 - percent : 50;
    $(`#bar-${key}`).style.left = `${start}%`;
    $(`#bar-${key}`).style.width = `${percent}%`;
    $(`#bar-${key}`).style.opacity = present ? "1" : "0";
  }
  const throttle = present ? sticks.throttle * 2 - 1 : 0;
  updateStick("left", 238, present ? sticks.yaw : 0, throttle);
  updateStick("right", 442, present ? sticks.roll : 0, present ? -sticks.pitch : 0);
  for (let axis = 4; axis <= 7; axis++) {
    const value = present && finite(sticks.axes[axis]) ? sticks.axes[axis] : null;
    text(`#aux-${axis}`, `A${axis} · ${fmt(value, 1)}`);
    const cx = [203, 266, 414, 477][axis - 4];
    $(`#switch-${axis}`).setAttribute("transform", `rotate(${value === null ? 0 : value * 13} ${cx} 79)`);
  }
  const destinations = { throttle: "频率", yaw: "相位", pitch: "中心角", roll: data.mode === 2 ? "差动" : "左右角差" };
  for (const key of Object.keys(channelNames)) text(`#label-${key}`, `A${data.config.roles[key]} · ${destinations[key]}`);
  renderButtons(data);
  const labels = ["上电锁定", "待命锁定", "已解锁", "安全回位", "已锁定", "故障锁定"];
  const stateLabel = status ? labels[status.state] || "未知状态" : "等待飞控";
  badge("#state-badge", stateLabel, status?.faults ? "danger" : status ? "" : "neutral");
  text("#safety-state", stateLabel);
  text("#safety-caption", data.demo ? "演示状态 · 非真实设备" : armed ? "输出已使能 · 操作时保持注意" : status ? "输出锁定 · 需明确按住解锁" : (snapshot?.reason || "连接后显示下位机状态"));
  $("#state-symbol").className = `state-symbol ${status?.faults ? 'danger' : status ? 'ready' : ''}`;
  $("#state-symbol").innerHTML = icon(armed ? "unlock" : "lock");
  text("#telemetry-indicator", connected ? `${fmt(snapshot.status_age * 1000)} ms` : "无遥测");
  for (const side of ["left", "right"]) {
    const angle = status ? status[`${side}_cd`] / 100 : null;
    setUnit(`#servo-${side}`, fmt(angle, 1), "°");
    const cx = side === "left" ? 91 : 269;
    $(`#servo-arm-${side}`).setAttribute("transform", `rotate(${clamp(angle || 0, -90, 90)} ${cx} 80)`);
    $(`#servo-arm-${side}`).style.opacity = status ? "1" : ".35";
  }
  setUnit("#phase-value", fmt(status ? status.phase_cd / 100 : null, 1), "°");
  $("#phase-pointer").setAttribute("transform", `rotate(${status ? status.phase_cd / 100 : 0} 180 81)`);
  const faults = status ? data.fault_labels : [];
  $("#fault-box").className = `fault-box ${status ? faults.length ? 'danger' : 'ok' : ''}`;
  text("#fault-title", status ? faults.length ? `检测到 ${faults.length} 项故障` : "故障检查正常" : "尚未读取故障");
  text("#fault-detail", status ? faults.length ? faults.join("、") : "下位机未报告故障位" : "等待下位机遥测");
  const voltage = status && finite(status.vbat_mv) ? status.vbat_mv / 1000 : null;
  const current = status?.iservo_ma;
  setUnit("#battery-value", fmt(voltage, 2), "V");
  setUnit("#current-value", fmt(current), "mA");
  const batteryPercent = voltage === null ? 0 : clamp((voltage - 3) / 1.2, 0, 1) * 100;
  $("#battery-fill").style.width = `${batteryPercent}%`;
  $("#battery-fill").style.background = status && status.faults & ((1 << 4) | (1 << 5)) ? "#ca6871" : "#7cb4a7";
  text("#battery-caption", voltage === null ? "未收到有效电压数据" : "电压条按单节 3.0–4.2 V 展示");
  $("#current-fill").style.width = `${finite(current) ? clamp(current / 2000, 0, 1) * 100 : 0}%`;
  text("#power-source", data.demo ? "模拟遥测" : connected ? "来自飞控 STATUS" : "等待遥测");
  setUnit("#status-age", fmt(connected ? snapshot.status_age * 1000 : null), "ms");
  setUnit("#rx-age", fmt(status?.rx_age_ms), "ms");
  setUnit("#lease-ms", fmt(status?.lease_ms), "ms");
  const command = data.command;
  badge("#command-badge", ["保持中位", "手动角度", "正弦扑翼"][command.mode] || "未知模式", command.mode ? "" : "neutral");
  setUnit("#cmd-frequency", fmt(command.frequency_hz, 3), "Hz");
  setUnit("#cmd-amplitude", `${fmt(command.amplitude_deg[0], 1)} / ${fmt(command.amplitude_deg[1], 1)}`, "°");
  setUnit("#cmd-phase", fmt(command.phase_deg, 1), "°");
  text("#cmd-differential", fmt(command.differential, 2));
  text("#command-issues", data.issues.length ? data.issues.join("；") : data.demo ? "模拟命令 · 不发送到设备" : !connected ? "尚未发送控制帧" : armed ? "命令已通过本地校验" : "未解锁，发送保持中位命令");
  for (const [selector, value] of [["#tx-count", snapshot?.tx], ["#rx-count", snapshot?.rx], ["#bad-count", snapshot?.bad], ["#reject-count", snapshot?.rejected]]) text(selector, String(value ?? 0));
  $$("[data-mode]").forEach(button => {
    button.classList.toggle("active", Number(button.dataset.mode) === data.mode);
    button.disabled = data.demo || data.observer || !connected || (Number(button.dataset.mode) !== 0 && !armed);
  });
  const blocked = data.demo || data.observer || !connected || data.blockers.length > 0 || armed;
  $("#arm").disabled = blocked;
  if (blocked && holding) cancelHold();
  text("#arm-blockers", data.blockers.length ? data.blockers.join("；") : armed ? "已解锁 · 上锁可立即停止输出" : "解锁条件满足，请连续按住 1.2 秒");
  $("#operation-dot").classList.toggle("warning", blocked && !armed);
  $("#disarm").disabled = data.demo || data.observer || !connected;
  $("#clear-faults").disabled = data.demo || data.observer || !connected || armed;
  $("#estop").disabled = data.demo;
  $("#mobile-estop").disabled = data.demo;
  $("#connect").disabled = data.demo || data.observer;
  $("#scan").disabled = data.demo || data.observer;
  $("#use-joystick").disabled = data.demo || data.observer || armed;
  $("#use-joystick").checked = data.config.mapping.use_joystick;
  text("#input-source-label", data.config.mapping.use_joystick ? "使用 USB 摇杆" : "使用界面滑块");
  text("#footer-session", connected ? `会话 ${snapshot.session.toString(16).padStart(8, '0')} · 序号 ${snapshot.sequence}` : "会话未建立");
  text("#log-count", String(data.logs.length));
  if (drawerType === "logs" && data.logs.at(-1)?.id !== lastLogId) renderLogs();
  if (drawerType === "mapping") renderRawAxes();
  if (drawerType === "commands") {
    renderCommandFrequency(data);
    renderServoTrim(data);
  }
  addHistory(data, status, present);
}

function updateStick(side, cx, horizontal, vertical) {
  const pixelX = cx + clamp(horizontal, -1, 1) * 25;
  const pixelY = 170 - clamp(vertical, -1, 1) * 25;
  $(`#stick-${side}`).setAttribute("transform", `translate(${pixelX} ${pixelY - 5})`);
  $(`#stem-${side}`).setAttribute("x2", String(pixelX));
  $(`#stem-${side}`).setAttribute("y2", String(pixelY - 1));
}

function renderButtons(data) {
  const count = data.device?.buttons || 16;
  const grid = $("#button-grid");
  if (grid.children.length !== count) {
    grid.innerHTML = Array.from({ length: count }, (_, index) => `<button class="hid-button" data-hid="${index}">B${index}</button>`).join("");
  }
  $$(".hid-button", grid).forEach((button, index) => {
    const pressed = data.sticks.connected && Boolean(data.sticks.buttons[index]);
    const assigned = data.config.roles.arm_button === index;
    const estop = data.config.roles.estop_button === index;
    button.classList.toggle("pressed", pressed);
    button.classList.toggle("assigned", assigned);
    button.classList.toggle("estop-assigned", estop);
    const purpose = assigned ? "按住解锁" : estop ? "急停" : "未分配控制功能";
    button.title = `USB 按键 B${index} · ${purpose} · ${pressed ? '按下' : '松开'}`;
    button.setAttribute("aria-label", button.title);
    button.setAttribute("aria-pressed", String(pressed));
  });
}

function addHistory(data, status, present) {
  const now = performance.now() / 1000;
  if (now - lastHistoryAt < 0.09) return;
  lastHistoryAt = now;
  history.push({ time: now, sticks: present ? { ...data.sticks } : null,
    left: status ? status.left_cd / 100 : null, right: status ? status.right_cd / 100 : null,
    voltage: status && finite(status.vbat_mv) ? status.vbat_mv / 1000 : null,
    current: status && finite(status.iservo_ma) ? status.iservo_ma / 1000 : null });
  while (history.length && history[0].time < now - 30) history.shift();
  drawChart();
}

const seriesDefinitions = {
  sticks: [
    { label: "油门", color: "#218e99", value: row => row.sticks?.throttle },
    { label: "偏航", color: "#609dc0", value: row => row.sticks?.yaw },
    { label: "俯仰", color: "#9ba1c6", value: row => row.sticks?.pitch },
    { label: "横滚", color: "#c7a576", value: row => row.sticks?.roll },
  ],
  servo: [
    { label: "左舵机指令 / °", color: "#218e99", value: row => row.left },
    { label: "右舵机指令 / °", color: "#7aa8c2", value: row => row.right },
  ],
  power: [
    { label: "电压 / V", color: "#218e99", value: row => row.voltage },
    { label: "电流 / A", color: "#7aa8c2", value: row => row.current },
  ],
};

function drawChart() {
  const canvas = $("#history-chart");
  const box = canvas.getBoundingClientRect();
  const ratio = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(box.width * ratio);
  canvas.height = Math.round(box.height * ratio);
  const context = canvas.getContext("2d");
  context.scale(ratio, ratio);
  const width = box.width, height = box.height;
  const left = 35, right = width - 12, top = 12, bottom = height - 24;
  if (right <= left || bottom <= top) return;
  const definitions = seriesDefinitions[chartType];
  const range = chartType === "sticks" ? [-1, 1] : chartType === "servo" ? [-90, 90] : [0, Math.max(5, ...history.map(row => row.voltage || 0))];
  const mapY = value => bottom - (value - range[0]) / (range[1] - range[0]) * (bottom - top);
  const now = performance.now() / 1000;
  const mapX = value => left + (value - (now - 30)) / 30 * (right - left);
  context.font = '9px "Segoe UI", "Microsoft YaHei", sans-serif';
  context.lineWidth = 1;
  for (let index = 0; index < 3; index++) {
    const value = range[0] + (range[1] - range[0]) * index / 2;
    const py = mapY(value);
    context.strokeStyle = "#eaf0f4";
    context.beginPath(); context.moveTo(left, py); context.lineTo(right, py); context.stroke();
    context.fillStyle = "#a0afb9"; context.textAlign = "right";
    context.fillText(String(Math.round(value * 10) / 10), left - 9, py + 3);
  }
  context.fillStyle = "#a0afb9";
  for (let index = 0; index <= 3; index++) {
    const px = left + (right - left) * index / 3;
    context.textAlign = index === 0 ? "left" : index === 3 ? "right" : "center";
    context.fillText(index === 3 ? "现在" : `-${30 - index * 10} s`, px, height - 5);
  }
  let anyData = false;
  context.save(); context.beginPath(); context.rect(left, top - 2, right - left, bottom - top + 4); context.clip();
  for (const series of definitions) {
    context.beginPath(); context.strokeStyle = series.color; context.lineWidth = 1.7;
    let drawing = false;
    for (const row of history) {
      const value = series.value(row);
      if (!finite(value)) { drawing = false; continue; }
      anyData = true;
      if (!drawing) { context.moveTo(mapX(row.time), mapY(value)); drawing = true; }
      else context.lineTo(mapX(row.time), mapY(value));
    }
    context.stroke();
  }
  context.restore();
  $("#chart-empty").hidden = anyData;
  text("#chart-empty", chartType === "sticks" ? "拨动 USB 摇杆，观察实时曲线" : "连接飞控后显示真实遥测曲线");
}

function setChart(type) {
  chartType = type;
  $$("[data-chart]").forEach(button => button.classList.toggle("active", button.dataset.chart === type));
  $("#chart-legend").innerHTML = seriesDefinitions[type].map(series => `<span><i style="background:${series.color}"></i>${series.label}</span>`).join("");
  drawChart();
}

function numberField(label, key, value, extra = "", step = "any") {
  return `<label class="field"><span>${label}</span><input type="number" name="${key}" value="${escapeHTML(value)}" step="${step}" ${extra}></label>`;
}

function roleField(key, label) {
  const roles = config.roles;
  const count = Math.max(state.device?.axes || 8, roles[key] + 1);
  return `<div class="settings-section"><h3>${label}</h3><label class="field"><span>USB 轴编号</span><select name="${key}">${Array.from({ length: count }, (_, index) => `<option value="${index}" ${roles[key] === index ? 'selected' : ''}>A${index}</option>`).join("")}</select></label><label class="field"><span>反向输入</span><input type="checkbox" name="invert_${key}" ${roles[`invert_${key}`] ? 'checked' : ''}></label></div>`;
}

function buttonOptions(selected) {
  return `<option value="">不分配</option>${Array.from({ length: state.device?.buttons || 16 }, (_, index) => `<option value="${index}" ${selected === index ? 'selected' : ''}>B${index}</option>`).join("")}`;
}

function openDrawer(type, detail = null) {
  if (!config) { toast("正在连接本机服务，请稍候", true); return; }
  drawerType = type;
  selectedButton = detail;
  const title = { mapping: "摇杆映射", settings: "参数与本地限幅", commands: "命令参数", logs: "事件记录" }[type];
  text("#drawer-title", title);
  text("#drawer-kicker", state.demo ? "演示预览 · 设置只读" : "BFCopilot / 本机设置");
  const content = $("#drawer-content");
  if (type === "mapping") {
    content.innerHTML = `${detail !== null ? `<div class="button-detail">已选中 USB 按键 B${detail}，可在下方分配解锁或急停。</div>` : ''}<p>标注描述 USB 输入与控制用途。辅助 A4–A7 对应的实体开关位置，请拨动实物确认；未分配时不触发飞控动作。</p><section class="settings-section"><h3>原始 USB 轴</h3><div id="raw-axes"></div></section><form id="roles-form">${Object.entries(channelNames).map(([key, label]) => roleField(key, label)).join("")}<section class="settings-section"><h3>油门行程校准</h3><p>先把油门拨到相应端点，再记录；保存到本机后下次启动生效。</p><p id="throttle-calibration">低位 ${fmt(config.roles.throttle_low, 3)} / 高位 ${fmt(config.roles.throttle_high, 3)}</p><button class="button secondary" type="button" data-calibrate="low">记录最低点</button><button class="button secondary" type="button" data-calibrate="high">记录最高点</button></section><section class="settings-section"><h3>实体按键功能</h3><label class="field"><span>解锁 · 先松开再按住 1 秒</span><select name="arm_button">${buttonOptions(config.roles.arm_button)}</select></label><label class="field"><span>急停 · 按下立即触发</span><select name="estop_button">${buttonOptions(config.roles.estop_button)}</select></label><p>B0–B15 为 USB 上报编号，不假定实体电源或绑定键会通过 USB 上报。</p></section><button class="button primary save-button">应用摇杆映射</button></form><button class="button secondary save-button" data-save>保存当前设置到本机</button>`;
    if (detail !== null) $("[name=arm_button]", content).focus();
    $("#roles-form").addEventListener("submit", async event => {
      event.preventDefault();
      const values = {};
      for (const key of Object.keys(channelNames)) {
        values[key] = Number($(`[name=${key}]`, content).value);
        values[`invert_${key}`] = $(`[name=invert_${key}]`, content).checked;
      }
      for (const key of ["arm_button", "estop_button"]) {
        const value = $(`[name=${key}]`, content).value;
        values[key] = value === "" ? null : Number(value);
      }
      if (values.arm_button !== null && values.arm_button === values.estop_button) {
        toast("解锁和急停请选择不同按键", true); return;
      }
      if (await action("configure", { group: "roles", values })) toast("摇杆映射已应用");
    });
    renderRawAxes();
  } else if (type === "commands") {
    const manual = config.manual;
    const joystick = config.mapping.use_joystick;
    const ranges = [
      ...(!joystick ? [["frequency_hz", "扑翼频率", "Hz", 0, config.limits.max_frequency_hz, 0.01]] : []),
      ["amplitude_left_deg", "左扑翼幅度", "°", 0, 90, 0.1],
      ["amplitude_right_deg", "右扑翼幅度", "°", 0, 90, 0.1],
      ["center_left_deg", "左中心角", "°", -90, 90, 0.1],
      ["center_right_deg", "右中心角", "°", -90, 90, 0.1],
      ["phase_deg", "相位偏移", "°", -180, 180, 0.1],
      ["differential", "差动", "", -1, 1, 0.01],
    ];
    const trimFields = [["trim_left_deg", "左中值偏移"], ["trim_right_deg", "右中值偏移"]];
    const trimForm = `<form id="servo-trim-form" class="settings-section"><h3>舵机中值微调</h3><p>偏移叠加到摇杆和手动中心角，最终输出仍受行程限幅。上锁后修改并应用，解锁后生效。</p>${trimFields.map(([key, label]) => `${numberField(`${label} / °`, key, config.mapping[key], 'min="-90" max="90" required', '0.1')}<div>${[-1, -0.1, 0.1, 1].map(step => `<button type="button" class="button secondary" data-trim="${key}" data-step="${step}" aria-label="${label} ${step > 0 ? '+' : ''}${step} 度">${step > 0 ? '+' : ''}${step}°</button>`).join("")}</div>`).join("")}<p>微调和归零只修改输入值；点击应用后生效。保存到本机后，重启上位机会恢复；更换电脑需要重新设置。</p><button type="button" class="button secondary" data-reset-trim>两侧归零</button><button type="submit" class="button primary">应用中值</button><button type="submit" class="button secondary save-button" data-save-trim>应用并保存到本机</button></form>`;
    content.innerHTML = `<p>参数经本地限幅后发送。使用摇杆时，油门决定当前频率，可在这里设置满油门频率和左右幅度。</p>${trimForm}${joystick ? `<form id="frequency-limit-form" class="settings-section">${numberField("满油门频率 / Hz", "max_frequency_hz", config.limits.max_frequency_hz, 'min="0.01" max="50" required')}<p>当前发送频率：<b id="command-frequency">${fmt(state.command.frequency_hz, 2)} Hz</b></p><p id="frequency-limit-hint">油门从 0% 到 100% 对应 0 到满油门频率；保持模式下发送 0 Hz。</p><button class="button primary">应用频率上限</button></form>` : ''}<section class="settings-section">${ranges.map(([key, label, unit, min, max, step]) => {
      const disabled = config.mapping.use_joystick && !key.startsWith("amplitude");
      return `<div class="range-field"><label for="manual-${key}"><span>${label}${disabled ? ' · 摇杆控制' : ''}</span><b id="manual-value-${key}">${fmt(manual[key], step < 0.1 ? 2 : 1)} ${unit}</b></label><input id="manual-${key}" type="range" data-manual="${key}" data-unit="${unit}" min="${min}" max="${max}" step="${step}" value="${manual[key]}" ${disabled ? 'disabled' : ''}></div>`;
    }).join("")}</section><p>保持或手动模式不输出扑翼频率。未解锁时，飞控命令保持中位。</p><button class="button secondary save-button" data-save>保存当前设置到本机</button>`;
    $$("[data-manual]", content).forEach(input => input.addEventListener("input", () => {
      const key = input.dataset.manual;
      text(`#manual-value-${key}`, `${Number(input.value).toFixed(2)} ${input.dataset.unit}`);
      scheduleChange("manual", key, Number(input.value));
    }));
    const trimFormElement = $("#servo-trim-form");
    $$("[data-trim]", trimFormElement).forEach(button => button.addEventListener("click", () => {
      const input = trimFormElement.elements[button.dataset.trim];
      const value = input.valueAsNumber;
      if (!Number.isFinite(value)) { input.reportValidity(); return; }
      input.value = clamp(value + Number(button.dataset.step), -90, 90).toFixed(1);
    }));
    $("[data-reset-trim]", trimFormElement).addEventListener("click", () => {
      trimFields.forEach(([key]) => { trimFormElement.elements[key].value = "0.0"; });
    });
    trimFormElement.addEventListener("submit", async event => {
      event.preventDefault();
      const save = event.submitter?.hasAttribute("data-save-trim");
      const values = Object.fromEntries(trimFields.map(([key]) => [key, trimFormElement.elements[key].valueAsNumber]));
      if (!await action("configure", { group: "mapping", values })) return;
      if (save) {
        if (await action("save")) toast("舵机中值已应用并保存到本机");
      } else toast("舵机中值已应用，尚未保存到本机");
    });
    renderServoTrim(state);
    if (joystick) {
      $("#frequency-limit-form").addEventListener("submit", async event => {
        event.preventDefault();
        const frequency = Number(event.currentTarget.elements.max_frequency_hz.value);
        if (await action("configure", { group: "limits", values: { max_frequency_hz: frequency } })) toast(`满油门频率已设为 ${fmt(frequency, 2)} Hz`);
      });
      renderCommandFrequency(state);
    }
  } else if (type === "settings") {
    content.innerHTML = `<p>这里修改的是上位机映射与本地限幅。下位机实际参数仍由飞控控制台维护；当前 UDP 协议只回传工作状态，不支持参数读写。</p><form id="mapping-form"><section class="settings-section"><h3>摇杆手感与映射</h3>${numberField("死区", "deadband", config.mapping.deadband, 'min="0" max="0.9"')}${numberField("指数", "expo", config.mapping.expo, 'min="0" max="1"')}${numberField("中心角范围 / °", "center_range_deg", config.mapping.center_range_deg, 'min="0" max="90"')}${numberField("相位范围 / °", "phase_range_deg", config.mapping.phase_range_deg, 'min="0" max="180"')}${numberField("差动范围", "differential_range", config.mapping.differential_range, 'min="0" max="1"')}<label class="field"><span>解锁要求 USB 摇杆在线</span><input type="checkbox" name="require_joystick" ${config.mapping.require_joystick ? 'checked' : ''}></label><button class="button primary">应用映射参数</button></section></form><form id="limits-form"><section class="settings-section"><h3>本地命令限幅</h3><p>飞控仍独立执行自己的参数与安全检查。</p>${numberField("最大频率 / Hz", "max_frequency_hz", config.limits.max_frequency_hz, 'min="0.01" max="50"')}${numberField("启动渐变 / s", "ramp_s", config.limits.ramp_s, 'min="0.01" max="60"')}${["min_deg", "max_deg", "speed_dps", "acceleration_dps2"].map((key, index) => [0, 1].map(side => numberField(`${side ? '右' : '左'}${['最小角度 / °', '最大角度 / °', '最大速度 / °/s', '最大加速度 / °/s²'][index]}`, `${key}_${side}`, config.limits[key][side])).join("")).join("")}<button class="button primary">应用本地限幅</button></section></form><button class="button secondary save-button" data-save>保存当前设置到本机</button>`;
    $("#mapping-form").addEventListener("submit", async event => {
      event.preventDefault();
      const form = event.currentTarget;
      const values = Object.fromEntries(["deadband", "expo", "center_range_deg", "phase_range_deg", "differential_range"].map(key => [key, Number(form.elements[key].value)]));
      values.require_joystick = form.elements.require_joystick.checked;
      if (await action("configure", { group: "mapping", values })) toast("映射参数已应用");
    });
    $("#limits-form").addEventListener("submit", async event => {
      event.preventDefault();
      const form = event.currentTarget;
      const values = { max_frequency_hz: Number(form.elements.max_frequency_hz.value), ramp_s: Number(form.elements.ramp_s.value) };
      for (const key of ["min_deg", "max_deg", "speed_dps", "acceleration_dps2"]) values[key] = [0, 1].map(side => Number(form.elements[`${key}_${side}`].value));
      if (await action("configure", { group: "limits", values })) toast("本地限幅已应用");
    });
  } else renderLogs();
  if (state.demo || state.observer) $$("input, select, button", content).forEach(element => { element.disabled = true; });
  hydrateIcons(content);
  if (!$("#settings-dialog").open) $("#settings-dialog").showModal();
}

function renderServoTrim(data) {
  const form = $("#servo-trim-form");
  if (!form) return;
  const armed = Boolean(data.snapshot?.status?.armed);
  $$("input, button", form).forEach(element => { element.disabled = data.demo || data.observer || armed; });
}

function renderCommandFrequency(data) {
  const form = $("#frequency-limit-form");
  if (!form) return;
  text("#command-frequency", `${fmt(data.command.frequency_hz, 2)} Hz`);
  const armed = Boolean(data.snapshot?.status?.armed);
  $$("input, button", form).forEach(element => { element.disabled = data.demo || data.observer || armed; });
  text("#frequency-limit-hint", armed ? "请先上锁，再修改满油门频率。" : "油门从 0% 到 100% 对应 0 到满油门频率；保持模式下发送 0 Hz。");
}

function renderRawAxes() {
  if (!$("#raw-axes")) return;
  const axes = state.sticks.connected ? state.sticks.axes : [];
  $("#raw-axes").innerHTML = axes.length ? axes.map((value, index) => `<div class="raw-axis"><span>A${index}</span><div><i style="left:calc(${(clamp(value, -1, 1) + 1) * 50}% - 3px)"></i></div><b>${signed(value)}</b></div>`).join("") : '<p>等待 USB 摇杆</p>';
  text("#throttle-calibration", `低位 ${fmt(state.config.roles.throttle_low, 3)} / 高位 ${fmt(state.config.roles.throttle_high, 3)}`);
}

function renderLogs() {
  if (!state) return;
  lastLogId = state.logs.at(-1)?.id || "";
  $("#drawer-content").innerHTML = `<p>记录连接、命令确认、拒绝原因和安全操作。保留最近 180 条事件。</p>${state.logs.length ? [...state.logs].reverse().map(event => `<div class="event-row ${/失败|急停|拒绝|中断/.test(event.text) ? 'error' : ''}"><time>${escapeHTML(event.time)}</time><span>${escapeHTML(event.text)}</span></div>`).join("") : '<div class="empty-logs">暂无事件，连接飞控后会在这里记录。</div>'}`;
}

async function startHold() {
  if (holding || $("#arm").disabled) return;
  holding = true;
  holdStarted = performance.now();
  const ok = await action("arm_hold");
  if (!ok || !holding) { cancelHold(); return; }
  holdTimer = setInterval(async () => {
    if (!holding || holdPending) return;
    holdPending = true;
    const alive = await action("arm_hold", {}, true);
    holdPending = false;
    if (!holding) return;
    if (!alive) { cancelHold(); return; }
    const progress = clamp((performance.now() - holdStarted) / 1350, 0, 1);
    $("#hold-progress").style.width = `${progress * 100}%`;
    text("#arm-label", "保持按住…");
    if (progress >= 1) {
      holding = false;
      clearInterval(holdTimer);
      $("#hold-progress").style.width = "0";
      text("#arm-label", "按住解锁");
      if (await action("arm")) toast("已提交解锁请求，等待飞控确认");
    }
  }, 120);
}

function cancelHold() {
  if (holding) action("arm_cancel", {}, true);
  holding = false;
  clearInterval(holdTimer);
  $("#hold-progress").style.width = "0";
  text("#arm-label", "按住解锁");
}

function emergencyStop() {
  cancelHold();
  if (state?.demo) return;
  action("estop").then(ok => { if (ok) toast("已请求急停；恢复控制需重新连接"); });
}

$(".brand").addEventListener("click", event => { event.preventDefault(); window.scrollTo({ top: 0, behavior: "smooth" }); });
$("#connection-form").addEventListener("submit", event => {
  event.preventDefault();
  cancelHold();
  if (state?.active) action("disconnect");
  else action("connect", { host: $("#host").value.trim(), port: Number($("#port").value) });
});
$("#scan").addEventListener("click", async () => {
  if (await action("scan", { host: $("#host").value.trim(), port: Number($("#port").value) })) toast("正在扫描同一热点内的飞控…");
});
$("#use-joystick").addEventListener("change", event => action("configure", { group: "mapping", values: { use_joystick: event.target.checked } }));
$("#chart-tabs").addEventListener("click", event => { if (event.target.dataset.chart) setChart(event.target.dataset.chart); });
$("#mode-buttons").addEventListener("click", event => { if (event.target.dataset.mode !== undefined) action("mode", { mode: Number(event.target.dataset.mode) }); });
$("#disarm").addEventListener("click", () => { cancelHold(); action("disarm"); });
$("#estop").addEventListener("click", emergencyStop);
$("#mobile-estop").addEventListener("click", emergencyStop);
$("#clear-faults").addEventListener("click", () => $("#clear-dialog").showModal());
$("#cancel-clear").addEventListener("click", () => $("#clear-dialog").close());
$("#confirm-clear").addEventListener("click", () => { $("#clear-dialog").close(); action("clear"); });
$("#arm").addEventListener("pointerdown", event => {
  if (event.button !== 0) return;
  event.preventDefault();
  event.currentTarget.setPointerCapture(event.pointerId);
  startHold();
});
for (const event of ["pointerup", "pointercancel", "lostpointercapture"]) $("#arm").addEventListener(event, cancelHold);
$("#arm").addEventListener("pointermove", event => {
  if (!holding) return;
  const box = event.currentTarget.getBoundingClientRect();
  if (event.clientX < box.left || event.clientX > box.right || event.clientY < box.top || event.clientY > box.bottom) cancelHold();
});
$("#arm").addEventListener("keydown", event => {
  if ([" ", "Enter"].includes(event.key)) { event.preventDefault(); if (!event.repeat) startHold(); }
});
$("#arm").addEventListener("keyup", event => { if ([" ", "Enter"].includes(event.key)) cancelHold(); });
window.addEventListener("blur", cancelHold);
document.addEventListener("visibilitychange", () => { if (document.hidden) cancelHold(); });
document.addEventListener("keydown", event => { if (event.key === "Escape") emergencyStop(); });
document.addEventListener("click", event => {
  const nav = event.target.closest("[data-nav]");
  const open = event.target.closest("[data-open]");
  const map = event.target.closest("[data-map], [data-switch]");
  const hid = event.target.closest("[data-hid]");
  const calibration = event.target.closest("[data-calibrate]");
  const save = event.target.closest("[data-save]");
  if (nav) {
    if (nav.dataset.nav === "home") { $("#settings-dialog").close(); window.scrollTo({ top: 0, behavior: "smooth" }); }
    else openDrawer(nav.dataset.nav);
  }
  if (open) openDrawer(open.dataset.open);
  if (map) openDrawer("mapping");
  if (hid) openDrawer("mapping", Number(hid.dataset.hid));
  if (calibration) action("calibrate", { which: calibration.dataset.calibrate });
  if (save) flushChanges().then(ok => ok && action("save")).then(ok => { if (ok) toast("设置已保存到本机"); });
});
$("#controller-figure").addEventListener("keydown", event => {
  if (["Enter", " "].includes(event.key) && event.target.closest(".map-target")) { event.preventDefault(); openDrawer("mapping"); }
});
$("#close-drawer").addEventListener("click", () => $("#settings-dialog").close());
$("#settings-dialog").addEventListener("close", () => { drawerType = ""; selectedButton = null; });
$("#settings-dialog").addEventListener("click", event => { if (event.target === event.currentTarget && event.clientX < event.currentTarget.getBoundingClientRect().left) event.currentTarget.close(); });
window.addEventListener("pagehide", () => {
  if (state?.active && !state.demo && !state.observer) fetch("/api/action", {
    method: "POST", headers, body: JSON.stringify({ action: "estop" }), keepalive: true,
  }).catch(() => {});
});
new ResizeObserver(drawChart).observe($(".chart-wrap"));
setChart("sticks");

async function poll() {
  try {
    const response = await fetch("/api/state", { headers, signal: AbortSignal.timeout(1000) });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "本机服务不可用");
    pollFailed = false;
    state = data;
    config = data.config;
    if (!initialized) {
      $("#host").value = config.host;
      $("#port").value = config.port;
      initialized = true;
    } else if (document.activeElement !== $("#host") && config.host && config.host !== $("#host").value && data.logs.some(event => event.text.includes(`发现飞控 ${config.host}`))) $("#host").value = config.host;
    $("#bridge-dot").classList.remove("off");
    text("#bridge-label", data.demo ? "演示服务" : "本机已连接");
    render(data);
  } catch (error) {
    cancelHold();
    pollFailed = true;
    $("#notice").hidden = false;
    $("#notice").classList.add("danger");
    text("#notice", token ? `本机服务连接中断：${error.message}。恢复后请重新连接飞控。` : "请使用启动终端打印的完整网页地址打开控制台。");
    $("#bridge-dot").classList.add("off");
    text("#bridge-label", "服务未连接");
    if (state) render({ ...state, bridge_ok: false, active: false, blockers: ["本机服务连接中断"], sticks: { ...state.sticks, connected: false } });
    for (const selector of ["#arm", "#connect", "#scan", "#disarm", "#clear-faults", "#use-joystick"]) $(selector).disabled = true;
  }
  setTimeout(poll, pollFailed ? 700 : 80);
}
poll();
