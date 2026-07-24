const state = { user: null, nextCursor: null, loading: false, expiryCode: null };
const byId = (id) => document.getElementById(id);

function showNotice(message, error = false) {
  const notice = byId("notice");
  notice.textContent = message;
  notice.classList.toggle("error", error);
  notice.classList.remove("hidden");
  window.clearTimeout(showNotice.timer);
  showNotice.timer = window.setTimeout(() => notice.classList.add("hidden"), 5000);
}

async function api(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  if (options.body) headers["Content-Type"] = "application/json";
  const response = await fetch(path, { credentials: "same-origin", ...options, headers });
  const text = await response.text();
  let body = null;
  if (text) {
    try { body = JSON.parse(text); } catch { body = { error: { message: text } }; }
  }
  if (!response.ok) {
    const error = new Error(body?.error?.message || `请求失败（${response.status}）`);
    error.status = response.status;
    error.code = body?.error?.code;
    throw error;
  }
  return body;
}

function setAuthenticated(user) {
  state.user = user;
  byId("auth-view").classList.toggle("hidden", Boolean(user));
  byId("workspace").classList.toggle("hidden", !user);
  byId("user-panel").classList.toggle("hidden", !user);
  byId("current-user").textContent = user ? user.username : "";
  if (!user) {
    state.expiryCode = null;
    byId("expiry-panel").classList.add("hidden");
    byId("statistics-panel").classList.add("hidden");
  }
}

async function restoreSession() {
  try {
    const user = await api("/api/me");
    setAuthenticated(user);
    await loadLinks(true);
  } catch (error) {
    if (error.status !== 401) showNotice(error.message, true);
    setAuthenticated(null);
  }
}

async function submitCredentials(event, route) {
  event.preventDefault();
  const form = event.currentTarget;
  const data = new FormData(form);
  try {
    const result = await api(route, {
      method: "POST",
      body: JSON.stringify({ username: data.get("username"), password: data.get("password") })
    });
    form.reset();
    setAuthenticated(result.user);
    showNotice(route.endsWith("register") ? "账号已创建。" : "已登录。", false);
    await loadLinks(true);
  } catch (error) { showNotice(error.message, true); }
}

function localDateToUtc(value) {
  return value ? new Date(value).toISOString().replace(/\.\d{3}Z$/, "Z") : null;
}

async function createLink(event) {
  event.preventDefault();
  const form = event.currentTarget;
  const data = new FormData(form);
  const payload = { url: data.get("url") };
  if (data.get("custom_code")) payload.custom_code = data.get("custom_code");
  if (data.get("expires_at")) payload.expires_at = localDateToUtc(data.get("expires_at"));
  try {
    const created = await api("/api/short-links", { method: "POST", body: JSON.stringify(payload) });
    form.reset();
    showNotice(`已创建 /s/${created.code}`);
    await loadLinks(true);
  } catch (error) { showNotice(error.message, true); }
}

function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, (char) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" })[char]);
}

function renderLink(record) {
  const card = document.createElement("article");
  card.className = "link-card";
  card.dataset.code = record.code;
  card.dataset.expiresAt = record.expires_at || "";
  const nextStatus = record.status === "active" ? "disabled" : "active";
  const actionLabel = record.status === "active" ? "禁用" : "恢复";
  card.innerHTML = `
    <div>
      <a class="link-code" href="${escapeHtml(record.short_url)}" target="_blank" rel="noreferrer">${escapeHtml(record.short_url)}</a>
      <p class="link-url" title="${escapeHtml(record.original_url)}">${escapeHtml(record.original_url)}</p>
      <p class="link-meta"><span class="status ${escapeHtml(record.status)}">${escapeHtml(record.status)}</span> · 创建于 ${escapeHtml(record.created_at)} · 过期：${escapeHtml(record.expires_at || "永不过期")}</p>
    </div>
    <div class="link-actions">
      <button class="button ghost" data-action="statistics" type="button">统计</button>
      <button class="button ${nextStatus === "disabled" ? "danger" : ""}" data-action="status" data-status="${nextStatus}" type="button">${actionLabel}</button>
      <button class="button ghost" data-action="expiry" type="button">设置过期</button>
    </div>`;
  return card;
}

async function loadLinks(reset = false) {
  if (state.loading) return;
  state.loading = true;
  try {
    if (reset) {
      state.nextCursor = null;
      byId("links-list").replaceChildren();
    }
    const params = new URLSearchParams({ limit: "20" });
    const status = byId("status-filter").value;
    if (status) params.set("status", status);
    if (state.nextCursor) params.set("cursor", state.nextCursor);
    const result = await api(`/api/short-links?${params}`);
    result.items.forEach((item) => byId("links-list").appendChild(renderLink(item)));
    state.nextCursor = result.next_cursor;
    byId("load-more-button").classList.toggle("hidden", !state.nextCursor);
    byId("empty-state").classList.toggle("hidden", byId("links-list").children.length > 0);
  } catch (error) {
    if (error.status === 401) setAuthenticated(null);
    showNotice(error.message, true);
  } finally { state.loading = false; }
}

async function updateLink(code, payload) {
  try {
    await api(`/api/short-links/${encodeURIComponent(code)}`, { method: "PUT", body: JSON.stringify(payload) });
    showNotice("链接已更新。");
    await loadLinks(true);
    return true;
  } catch (error) {
    showNotice(error.message, true);
    return false;
  }
}

function openExpiryPanel(card) {
  state.expiryCode = card.dataset.code;
  byId("expiry-title").textContent = `/s/${state.expiryCode}`;
  byId("expiry-form").elements.expires_at.value = card.dataset.expiresAt;
  byId("expiry-panel").classList.remove("hidden");
  byId("expiry-panel").scrollIntoView({ behavior: "smooth", block: "nearest" });
}

function closeExpiryPanel() {
  state.expiryCode = null;
  byId("expiry-panel").classList.add("hidden");
}

async function showStatistics(code) {
  try {
    const result = await api(`/api/short-links/${encodeURIComponent(code)}/statistics`);
    byId("statistics-title").textContent = `/s/${code}`;
    byId("statistics-content").innerHTML = `
      <p class="link-meta">统计通过 Kafka 异步更新，短暂延迟属于正常现象。</p>
      <div class="stats-grid">
        <div class="stat">成功访问<strong>${result.summary.access_count}</strong></div>
        <div class="stat">访问尝试<strong>${result.summary.attempt_count}</strong></div>
        <div class="stat">最近访问<strong>${escapeHtml(result.summary.last_access_at || "暂无")}</strong></div>
      </div>`;
    byId("statistics-panel").classList.remove("hidden");
    byId("statistics-panel").scrollIntoView({ behavior: "smooth", block: "nearest" });
  } catch (error) { showNotice(error.message, true); }
}

byId("login-form").addEventListener("submit", (event) => submitCredentials(event, "/api/auth/login"));
byId("register-form").addEventListener("submit", (event) => submitCredentials(event, "/api/auth/register"));
byId("create-form").addEventListener("submit", createLink);
byId("refresh-button").addEventListener("click", () => loadLinks(true));
byId("status-filter").addEventListener("change", () => loadLinks(true));
byId("load-more-button").addEventListener("click", () => loadLinks(false));
byId("close-statistics").addEventListener("click", () => byId("statistics-panel").classList.add("hidden"));
byId("cancel-expiry").addEventListener("click", closeExpiryPanel);
byId("expiry-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!state.expiryCode) return;
  const value = new FormData(event.currentTarget).get("expires_at").trim();
  if (await updateLink(state.expiryCode, { expires_at: value || null })) closeExpiryPanel();
});
byId("logout-button").addEventListener("click", async () => {
  try { await api("/api/auth/session", { method: "DELETE" }); } catch (_) {}
  setAuthenticated(null);
  showNotice("已退出。", false);
});
byId("links-list").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-action]");
  if (!button) return;
  const card = button.closest(".link-card");
  const code = card.dataset.code;
  if (button.dataset.action === "statistics") showStatistics(code);
  if (button.dataset.action === "status") updateLink(code, { status: button.dataset.status });
  if (button.dataset.action === "expiry") openExpiryPanel(card);
});

restoreSession();
