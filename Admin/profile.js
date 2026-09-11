import { fillLocales, translate } from "/accounts-assets/i18n.js";

export function createProfile({ container, api, language, text, options, getProfile, refreshProfile, showLogin, run, notice }) {
  container.replaceChildren(document.getElementById("profile-template").content.cloneNode(true));
  translate(container, language);
  const byId = id => container.querySelector(`#${id}`);
  const fields = form => Object.fromEntries(new FormData(form).entries());
  const events = new AbortController();
  const on = (id, type, handler) => byId(id).addEventListener(type, handler, { signal: events.signal });
  function update({ user, locales }) {
    byId("identity").textContent = `${user.email} · ${text[user.role]}`;
    const profile = byId("profile-form");
    profile.elements.displayName.value = user.displayName;
    profile.elements.phone.value = user.phone;
    fillLocales(profile.elements.locale, locales, user.locale, language);
    byId("email-section").hidden = !options.email;
    byId("email-status").textContent = user.emailVerified ? text.verified : text.unverified;
    byId("verify-email").hidden = user.emailVerified;
    byId("email-form").hidden = !user.emailVerified;
    byId("subscription-section").hidden = !options.newsletters;
    byId("subscription-status").textContent = user.newsletterSubscribed ? text.subscribed : text.unsubscribed;
    byId("subscription").textContent = user.newsletterSubscribed ? text.unsubscribe : text.subscribe;
    byId("subscription").hidden = !user.emailVerified;
  }
  on("profile-form", "submit", event => {
    event.preventDefault();
    run(async () => {
      await api.request("/api/v1/me", { method: "PATCH", body: { ...fields(event.target), version: getProfile().user.version } });
      await refreshProfile(); notice(text.saved);
    });
  });
  on("password-form", "submit", event => {
    event.preventDefault();
    run(async () => {
      const body = fields(event.target); event.target.reset();
      await api.request("/api/v1/me/password", { method: "POST", body });
      showLogin(); notice(text.passwordChanged);
    });
  });
  on("verify-email", "click", () => run(async () => {
    await api.request("/api/v1/me/verify-email", { method: "POST", body: {} });
    notice(text.emailRequested);
  }));
  on("email-form", "submit", event => {
    event.preventDefault();
    run(async () => {
      const body = fields(event.target); event.target.elements.currentPassword.value = "";
      await api.request("/api/v1/me/email", { method: "POST", body });
      notice(text.emailRequested);
    });
  });
  on("subscription", "click", () => run(async () => {
    const subscribed = !getProfile().user.newsletterSubscribed;
    await api.request("/api/v1/me/newsletter", { method: "POST", body: { subscribed } });
    await refreshProfile(); notice(subscribed ? text.emailRequested : text.unsubscribed);
  }));
  return {
    load() { update(getProfile()); }, update,
    dispose() {
      events.abort();
      container.querySelectorAll("form").forEach(form => form.reset());
      container.replaceChildren();
    }
  };
}
