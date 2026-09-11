// Only shipped modules can be loaded. Configuration never supplies script URLs.
// Visibility is presentation; every API keeps enforcing its own authorization.
export function accountAccess(user, features = {}) {
  return {
    canBook: user?.role === "customer" && features.reservations === true,
    canManage: user?.role === "admin" || (user?.role === "operator" && features.reservations === true)
  };
}

const administrator = context => context.isAdminPage && context.getProfile().user.role === "admin";
export const accountModules = [
  {
    id: "reservations", title: "reservations", enabled: context => {
      const access = accountAccess(context.getProfile().user, context.features);
      return context.features.reservations === true && (context.isAdminPage ? access.canManage : access.canBook);
    },
    async create(context) {
      const { createReservations } = await import("/accounts-assets/reservations.js");
      return createReservations(context);
    }
  },
  {
    id: "services", title: "serviceMenu",
    enabled: context => administrator(context) && context.features.contentEditor,
    async create(context) {
      const { createServiceEditor } = await import("/accounts-assets/content.js");
      const card = document.createElement("section"), heading = document.createElement("h2"), body = document.createElement("div");
      heading.textContent = context.text.serviceMenu;
      card.append(heading, body); context.container.append(card);
      return createServiceEditor({ ...context, container: body });
    }
  },
  {
    id: "newsletters", title: "newsletters",
    enabled: context => administrator(context) && context.options.newsletters,
    async create(context) {
      const { createNewsletters } = await import("/accounts-assets/newsletters.js");
      const card = document.createElement("section"); context.container.append(card);
      return createNewsletters({ ...context, container: card, locales: context.getProfile().locales });
    }
  },
  {
    id: "mail", title: "mail",
    enabled: context => administrator(context) && context.options.email,
    async create(context) {
      const { createMail } = await import("/accounts-assets/mail.js");
      const card = document.createElement("section"); context.container.append(card);
      return createMail({ ...context, container: card });
    }
  },
  {
    id: "users", title: "users", enabled: administrator,
    async create(context) {
      const { createUsers } = await import("/accounts-assets/users.js");
      return createUsers(context);
    }
  },
  {
    id: "profile", title: "profile",
    enabled: context => !context.isAdminPage || accountAccess(context.getProfile().user, context.features).canManage,
    async create(context) {
      const { createProfile } = await import("/accounts-assets/profile.js");
      return createProfile(context);
    }
  }
];

// Modules expose load(), dispose(), and optionally update(profile). Existing
// instances survive profile refreshes so another section's drafts are preserved.
export function createModuleHost({ container, navigation, context, browser = window }, definitions = accountModules) {
  const active = new Map();
  let owner = null, generation = 0, listening = false;
  function select(normalize = true) {
    const requested = browser.location.hash;
    const available = definitions.filter(definition => definition.enabled(context)).map(definition => definition.id);
    // Keep a valid destination selected even while its module is still loading.
    const selected = available.find(id => requested === `#section-${id}`) ?? available[0];
    for (const [id, entry] of active) {
      entry.panel.hidden = id !== selected;
      entry.link.setAttribute("aria-current", id === selected ? "page" : "false");
    }
    // A stale/forbidden deep link falls back to the first available section.
    if (normalize && selected && requested !== `#section-${selected}`) {
      browser.history.replaceState(null, "", `#section-${selected}`);
    }
  }
  const navigate = () => select();
  function remove(id) {
    const entry = active.get(id);
    if (!entry) return;
    active.delete(id);
    entry.module?.dispose(); entry.panel.remove(); entry.link.remove();
  }
  function dispose() {
    generation++;
    browser.removeEventListener("hashchange", navigate);
    browser.removeEventListener("popstate", navigate);
    listening = false;
    for (const id of active.keys()) remove(id);
    owner = null;
  }
  async function sync() {
    const profile = context.getProfile();
    if (owner !== profile.user.id) { dispose(); owner = profile.user.id; }
    if (!listening) {
      browser.addEventListener("hashchange", navigate);
      browser.addEventListener("popstate", navigate);
      listening = true;
    }
    const current = ++generation;
    const enabled = definitions.filter(definition => definition.enabled(context));
    const wanted = new Set(enabled.map(definition => definition.id));
    for (const id of active.keys()) if (!wanted.has(id)) remove(id);
    for (const definition of enabled) {
      if (current !== generation) return;
      const existing = active.get(definition.id);
      if (existing) { existing.module?.update?.(profile); continue; }
      const panel = document.createElement("article"), link = document.createElement("a");
      panel.id = `section-${definition.id}`; panel.className = "module-panel";
      panel.hidden = true;
      link.href = `#${panel.id}`; link.textContent = context.text[definition.title];
      link.id = `navigation-${definition.id}`;
      link.setAttribute("aria-controls", panel.id); panel.setAttribute("aria-labelledby", link.id);
      link.addEventListener("click", event => {
        if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
        event.preventDefault();
        if (browser.location.hash !== `#${panel.id}`) browser.history.pushState(null, "", `#${panel.id}`);
        select(false); browser.scrollTo({ top: 0, behavior: "instant" });
      });
      container.append(panel); navigation.append(link);
      let module;
      try {
        module = await definition.create({ ...context, container: panel });
        if (current !== generation) { module.dispose(); panel.remove(); link.remove(); return; }
        active.set(definition.id, { module, panel, link });
        select(false);
        await module.load();
      } catch (error) {
        if (active.has(definition.id) && active.get(definition.id).module === module) remove(definition.id);
        else { panel.remove(); link.remove(); }
        if (current !== generation) return;
        if (error.status === 401 || error.status === 403) throw error;
        // One unavailable feature must not prevent access to the other sections.
        const card = document.createElement("section"), heading = document.createElement("h2"), message = document.createElement("p"), retry = document.createElement("button");
        heading.textContent = context.text[definition.title];
        message.textContent = context.text.connectionError; message.setAttribute("role", "alert");
        retry.type = "button"; retry.textContent = context.text.retry;
        retry.addEventListener("click", () => context.run(async () => { remove(definition.id); await sync(); }));
        card.append(heading, message, retry); panel.replaceChildren(card);
        container.append(panel); navigation.append(link);
        active.set(definition.id, { module: null, panel, link });
        select(false);
      }
    }
    // A re-enabled section goes back to its declared position.
    if (current === generation) for (const definition of enabled) {
      const entry = active.get(definition.id);
      container.append(entry.panel); navigation.append(entry.link);
    }
    if (current === generation) select();
  }
  return { sync, dispose };
}
