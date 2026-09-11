import { AccountApi, ApiError } from "/accounts-assets/api.js";
import { applyPresentation, fillLocales, messages, resolveLanguage } from "/accounts-assets/i18n.js";

const params = new URLSearchParams(location.search);
const fragment = new URLSearchParams(location.hash.slice(1));
const token = fragment.get("token");
const action = fragment.get("action") || params.get("action") || "register";
// Capability links stay out of HTTP URLs, referrers and the visible history.
history.replaceState(null, "", location.pathname + location.search);
let language = resolveLanguage(), text = messages("email", language);
const api = new AccountApi();
const root = document.getElementById("flow");
const status = document.getElementById("status");
function element(tag, value) { const node = document.createElement(tag); if (value !== undefined) node.textContent = value; return node; }
function input(parent, label, properties) {
  const wrapper = element("label", label), node = Object.assign(element("input"), properties);
  wrapper.append(node); parent.append(wrapper); return node;
}
function notice(message, error = false) { status.textContent = message; status.classList.toggle("error", error); }

async function initialize() {
  const options = await api.request("/api/v1/auth/options");
  language = resolveLanguage(options.ui); text = messages("email", language);
  for (const [id, label, path] of [["website", text.website, "/"], ["sign-in", text.signIn, "/profile"]]) {
    const link = document.getElementById(id); link.textContent = label; link.href = `${path}?lang=${language}`;
  }

  const title = action === "register" ? text.register : action === "reset" ? text.reset : action === "unsubscribe" ? text.unsubscribe : text.confirm;
  applyPresentation(options.ui, language, title); document.getElementById("title").textContent = title;
  if (!options.email || (action === "register" && !options.registration)) { notice(text.disabled, true); return; }
  const form = element("form"); form.method = "post";
  const fields = element("fieldset"); fields.className = "editor-fields";
  let email, name, phone, password, repeat, consent, deliveryLocale;
  if (!token) {
    if (action !== "register" && action !== "reset") { notice(text.error, true); return; }
    if (action === "register") root.append(element("p", text.help));
    email = input(fields, text.email, { type: "email", required: true, maxLength: 254, autocomplete: "email" });
    const localeLabel = element("label", messages("accounts", language).preferredLanguage);
    deliveryLocale = element("select");
    fillLocales(deliveryLocale, options.locales, params.get("lang") || language, language);
    localeLabel.append(deliveryLocale); fields.append(localeLabel);
  } else if (action === "register" || action === "reset") {
    if (action === "register") {
      name = input(fields, text.name, { required: true, maxLength: 100, autocomplete: "name" });
      phone = input(fields, text.phone, { type: "tel", maxLength: 32, autocomplete: "tel" });
    }
    password = input(fields, text.password, { type: "password", minLength: 15, maxLength: 128, required: true, autocomplete: "new-password" });
    repeat = input(fields, text.repeat, { type: "password", minLength: 15, maxLength: 128, required: true, autocomplete: "new-password" });
    if (action === "register" && options.newsletters) {
      consent = input(fields, text.consent, { type: "checkbox", checked: false }); consent.parentElement.className = "checkbox-field";
    }
  } else root.append(element("p", text.confirmHelp));
  const submit = element("button", token ? text.finish : text.send); fields.append(submit); form.append(fields); root.append(form);
  form.addEventListener("submit", async event => {
    event.preventDefault();
    if (fields.disabled) return;
    if (password && password.value !== repeat.value) { notice(text.mismatch, true); return; }
    let path, body;
    if (!token) { path = action === "register" ? "register" : "forgot-password"; body = { email: email.value.trim(), locale: deliveryLocale.value }; }
    else if (action === "register") { path = "complete-registration"; body = { token, displayName: name.value.trim(), phone: phone.value.trim(), password: password.value, newsletter: consent?.checked || false }; }
    else if (action === "reset") { path = "reset-password"; body = { token, password: password.value }; }
    else { path = "confirm-email"; body = { token }; }
    const endpoint = action === "unsubscribe" ? "/api/v1/newsletter/unsubscribe" : `/api/v1/auth/${path}`;
    fields.disabled = true;
    try {
      await api.request(endpoint, { method: "POST", body });
      root.replaceChildren();
      notice(action === "unsubscribe" ? text.removed : token ? text.done : text.requested);
    } catch (error) { notice(error instanceof ApiError ? error.message : text.error, true); }
    finally { fields.disabled = false; }
  });
}
initialize().catch(() => notice(text.error, true));
