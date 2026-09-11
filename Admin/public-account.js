import { AccountApi, ApiError } from "/accounts-assets/api.js";
import { messages, accountIdentity, resolveLanguage } from "/accounts-assets/i18n.js";
import { accountAccess } from "/accounts-assets/modules.js";

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function link(text, href) {
  const node = element("a", "text-link", text);
  node.href = href;
  return node;
}

function loginField(form, label, name, type, autocomplete, maximum) {
  const wrapper = element("label", "", label), input = element("input");
  Object.assign(input, { name, type, autocomplete, maxLength: maximum, required: true });
  wrapper.append(input);
  form.append(wrapper);
  return input;
}

// One controller per mounted header. It owns UI/session state; credentials are
// still sent only through AccountApi and the server's HttpOnly session cookie.
class PublicAccountControls {
  api = new AccountApi();
  sessionApi = new AccountApi();
  events = new AbortController();
  dialogEvents = new AbortController();
  user = null;
  busy = false;
  refreshing = false;
  revision = 0;
  disposed = false;

  constructor({ navigation, dialog, profileLink, features, onChange = () => {} }, options) {
    Object.assign(this, { navigation, dialog, profileLink, features, onChange, options });
    this.on(window, "pageshow", () => this.refresh());
    this.on(window, "focus", () => this.refresh());
  }

  on(target, event, callback, signal = this.events.signal) {
    target.addEventListener(event, callback, { signal });
  }

  url(path, section = "") {
    return `${path}?lang=${encodeURIComponent(this.language)}${section ? `#section-${section}` : ""}`;
  }

  action(label, callback) {
    const button = element("button", "account-action", label);
    button.type = "button";
    // These buttons are replaced on refresh and need no global listeners.
    button.addEventListener("click", callback);
    return button;
  }

  setLanguage(requested) {
    const language = resolveLanguage(this.options.ui, requested);
    if (this.disposed || this.language === language) return;
    this.language = language;
    this.copy = messages("accounts", language);
    this.buildDialog();
    this.render();
  }

  buildDialog() {
    this.dialogEvents.abort();
    this.dialogEvents = new AbortController();
    const { copy, dialog } = this, signal = this.dialogEvents.signal;
    const title = element("h2", "", copy.login);
    title.id = `${dialog.id}-title`;
    dialog.setAttribute("aria-labelledby", title.id);
    const close = element("button", "dialog-close", copy.close);
    close.type = "button";
    this.on(close, "click", () => dialog.close(), signal);
    const form = element("form", "login-form");
    form.method = "post";
    this.email = loginField(form, copy.email, "email", "email", "username", 254);
    this.password = loginField(form, copy.password, "password", "password", "current-password", 128);
    this.status = element("p");
    this.status.setAttribute("role", "status");
    this.status.setAttribute("aria-live", "polite");
    this.submit = element("button", "button button-filled", copy.login);
    this.submit.type = "submit";
    this.submit.disabled = this.busy;
    form.append(this.submit, this.status);
    const links = element("div", "account-navigation");
    if (this.options.registration) links.append(link(copy.register, `${this.url("/email")}&action=register`));
    if (this.options.email) links.append(link(copy.recover, `${this.url("/email")}&action=reset`));
    dialog.replaceChildren(close, title, form, links);
    this.on(dialog, "close", () => { this.password.value = ""; this.status.textContent = ""; }, signal);
    this.on(form, "submit", event => { event.preventDefault(); this.login(); }, signal);
  }

  render() {
    if (this.disposed) return;
    const { navigation, copy, user } = this;
    navigation.replaceChildren();
    navigation.hidden = false;
    if (!user) {
      navigation.append(this.action(copy.login, () => { this.dialog.showModal(); this.email.focus(); }));
    } else {
      const { canBook, canManage } = accountAccess(user, this.features);
      navigation.append(element("span", "session-identity", accountIdentity(user, this.language)),
        link(copy.profile, this.url("/profile", "profile")));
      if (canBook) navigation.append(link(messages("reservations", this.language).mine, this.url("/profile", "reservations")));
      if (canManage) navigation.append(link(copy.administration, this.url("/admin", user.role === "operator" ? "reservations" : "")));
      navigation.append(this.action(copy.logout, () => this.logout()));
    }
    if (this.profileLink) {
      this.profileLink.href = this.url("/profile", "profile");
      this.profileLink.textContent = copy.profile;
      this.profileLink.hidden = false;
    }
    this.onChange(user);
  }

  async refresh() {
    if (this.disposed || this.refreshing || this.busy) return;
    this.refreshing = true;
    const revision = this.revision;
    try {
      // A read started before login/logout must never overwrite the new identity
      // or its CSRF token. Keep reads separate from the mutation API instance.
      const session = await this.sessionApi.request("/api/v1/me");
      if (!this.disposed && revision === this.revision) {
        this.user = session.user;
        this.api.csrfToken = session.csrfToken;
      }
    } catch (error) {
      if (revision === this.revision && error instanceof ApiError && error.status === 401) this.user = null;
    } finally {
      this.refreshing = false;
      this.render();
    }
  }

  async login() {
    if (this.disposed || this.busy) return;
    this.busy = true;
    this.revision++;
    this.submit.disabled = true;
    this.status.textContent = "";
    const body = { email: this.email.value, password: this.password.value };
    this.password.value = "";
    try {
      const result = await this.api.request("/api/v1/auth/login", { method: "POST", body });
      if (this.disposed) return;
      this.user = result.user;
      this.dialog.close();
      this.render();
    } catch (error) {
      if (!this.disposed) this.status.textContent = error instanceof ApiError ? error.message : this.copy.connectionError;
    } finally {
      this.busy = false;
      this.submit.disabled = false;
    }
  }

  async logout() {
    if (this.disposed || this.busy) return;
    this.busy = true;
    this.revision++;
    try {
      await this.api.request("/api/v1/auth/logout", { method: "POST", body: {} });
      this.user = null;
      this.render();
    } catch {
      if (!this.disposed) location.assign(this.url("/profile"));
    } finally { this.busy = false; }
  }

  dispose() {
    this.disposed = true;
    this.revision++;
    this.events.abort();
    this.dialogEvents.abort();
    this.dialog.close();
    this.dialog.replaceChildren();
    this.navigation.replaceChildren();
    this.navigation.hidden = true;
    if (this.profileLink) this.profileLink.hidden = true;
  }
}

export async function mountPublicAccount(settings) {
  if (settings.features.accounts !== true) return null;
  const options = await new AccountApi().request("/api/v1/auth/options");
  const controls = new PublicAccountControls(settings, options);
  controls.setLanguage(settings.language);
  await controls.refresh();
  return controls;
}
