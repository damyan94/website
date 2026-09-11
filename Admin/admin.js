import { AccountApi, ApiError } from "/accounts-assets/api.js";
import { applyPresentation, fillLocales, interfaceLocales, messages, resolveLanguage, translate, accountIdentity } from "/accounts-assets/i18n.js";
import { accountAccess, createModuleHost } from "/accounts-assets/modules.js";

const api = new AccountApi();
const byId = id => document.getElementById(id);
const isAdminPage = location.pathname === "/admin";
let language = resolveLanguage(), text = messages("accounts", language);
let profile = null, host = null, options = null, features = null, busy = false;

function notice(message, error = false) {
  byId("status").textContent = message;
  byId("status").classList.toggle("error", error);
}
function showLogin() {
  host?.dispose(); profile = null; api.csrfToken = "";
  byId("login-section").hidden = false;
  for (const id of ["signed-in", "section-nav", "logout", "admin-link", "session-identity"]) byId(id).hidden = true;
  byId("session-identity").textContent = "";
  byId("login-form").elements.password.value = "";
}
async function run(operation) {
  if (busy) return;
  busy = true; notice("");
  // Preserve existing disabled controls (for example an account's own role).
  const controls = [...document.querySelectorAll("button, #language")].map(control => [control, control.disabled]);
  for (const [control] of controls) control.disabled = true;
  document.body.setAttribute("aria-busy", "true");
  try { await operation(); }
  catch (error) {
    if (error instanceof ApiError && error.status === 401) showLogin();
    notice(error instanceof ApiError ? error.message : text.connectionError, true);
  } finally {
    busy = false; document.body.removeAttribute("aria-busy");
    for (const [control, disabled] of controls) control.disabled = disabled;
  }
}
function presentation() {
  language = resolveLanguage(options.ui); text = messages("accounts", language);
  translate(document, language);
  const title = isAdminPage ? text.administration : text.profile;
  applyPresentation(options.ui, language, title); byId("title").textContent = title;
  document.querySelectorAll('a[href="/profile"], a[href="/admin"], a[href="/"]').forEach(link => {
    const url = new URL(link.href); url.searchParams.set("lang", language);
    if (url.pathname === "/profile") url.hash = "section-profile";
    link.href = url.href;
  });
  fillLocales(byId("language"), interfaceLocales, language, language);
  byId("section-nav").setAttribute("aria-label", text.sections);
  byId("register-link").hidden = !options.registration;
  byId("register-link").href = `/email?action=register&lang=${language}`;
  byId("recovery-link").hidden = !options.email;
  byId("recovery-link").href = `/email?action=reset&lang=${language}`;
}
async function refreshProfile(latest = null) {
  profile = latest ?? await api.request("/api/v1/me");
  const { canManage } = accountAccess(profile.user, features);
  if (isAdminPage && !canManage) {
    host?.dispose();
    byId("signed-in").hidden = true; byId("admin-link").hidden = true;
    location.replace(`/profile?lang=${language}#section-profile`);
    return;
  }
  byId("login-section").hidden = true;
  byId("signed-in").hidden = false;
  byId("section-nav").hidden = false;
  byId("logout").hidden = false;
  byId("admin-link").hidden = !canManage;
  byId("session-identity").textContent = accountIdentity(profile.user, language);
  byId("session-identity").hidden = false;
  await host.sync();
}
async function initialize() {
  if (host) return;
  [options, features] = await Promise.all([api.request("/api/v1/auth/options"), api.request("/api/v1/features")]);
  presentation();
  host = createModuleHost({ container: byId("sections"), navigation: byId("section-nav"), context: {
    api, language, text, options, features, isAdminPage, getProfile: () => profile, refreshProfile, showLogin, run, notice
  } });
}
byId("login-form").addEventListener("submit", event => {
  event.preventDefault();
  const body = Object.fromEntries(new FormData(event.target).entries());
  event.target.elements.password.value = "";
  run(async () => {
    await initialize();
    await api.request("/api/v1/auth/login", { method: "POST", body });
    await refreshProfile();
  });
});
byId("logout").addEventListener("click", () => run(async () => {
  await api.request("/api/v1/auth/logout", { method: "POST", body: {} });
  showLogin(); notice(text.signedOut);
}));
byId("language").addEventListener("change", event => {
  const url = new URL(location.href); url.searchParams.set("lang", event.target.value);
  location.assign(url.href);
});
function checkSession() {
  if (!profile || busy) return;
  run(async () => {
    const latest = await api.request("/api/v1/me");
    if (latest.user.id !== profile.user.id || latest.user.role !== profile.user.role) await refreshProfile(latest);
    else byId("session-identity").textContent = accountIdentity(latest.user, language);
    // An unchanged session leaves all editor drafts and their original versions intact.
  });
}
window.addEventListener("focus", checkSession);
window.addEventListener("pageshow", event => { if (event.persisted) checkSession(); });
// Reserve the actual ribbon height for deep links and keyboard focus, including
// wrapped navigation and status text on small screens.
new ResizeObserver(entries => {
  document.documentElement.style.setProperty("--ribbon-height", `${entries[0].target.offsetHeight}px`);
}).observe(byId("admin-ribbon"));
run(async () => {
  await initialize();
  try { await refreshProfile(); }
  catch (error) {
    if (!(error instanceof ApiError && error.status === 401)) throw error;
    showLogin();
  }
});
