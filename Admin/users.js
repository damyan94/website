import { fillLocales, translate } from "/accounts-assets/i18n.js";

export function createUsers({ container, api, language, text, getProfile, run, notice }) {
  container.replaceChildren(document.getElementById("users-template").content.cloneNode(true));
  translate(container, language);
  const byId = id => container.querySelector(`#${id}`);
  const events = new AbortController();
  const on = (id, type, handler) => byId(id).addEventListener(type, handler, { signal: events.signal });
  let page = 1, pages = 1, disposed = false;
  let filters = new URLSearchParams(new FormData(byId("user-filters")));
  const dates = new Intl.DateTimeFormat(language, { dateStyle: "medium", timeStyle: "short" });
  for (const element of container.querySelectorAll("[data-label]")) element.setAttribute("aria-label", text[element.dataset.label]);
  fillLocales(byId("create-form").elements.locale, getProfile().locales, getProfile().user.locale, language);
  function node(tag, value) {
    const result = document.createElement(tag);
    if (value !== undefined) result.textContent = value;
    return result;
  }
  function dateCell(value) {
    const cell = node("td");
    if (!value) { cell.textContent = text.noRecordedLogin; return cell; }
    const time = node("time", dates.format(new Date(value)));
    time.dateTime = value; time.title = value;
    cell.append(time); return cell;
  }
  function userRow(user) {
    const row = node("tr"), name = node("th", user.displayName);
    name.scope = "row";
    row.append(name, node("td", user.email), node("td", user.emailVerified ? text.emailConfirmed : text.emailUnconfirmed), dateCell(user.createdAt), dateCell(user.lastLoginAt));
    const form = node("form"); form.method = "post";
    form.setAttribute("aria-label", `${text.management}: ${user.email}`);
    const roleLabel = node("label", text.role), role = node("select");
    for (const value of ["customer", "operator", "admin"]) role.add(new Option(text[value], value));
    role.value = user.role; roleLabel.append(role);
    const accessLabel = node("label", text.access), enabled = node("select");
    enabled.add(new Option(text.enabled, "true")); enabled.add(new Option(text.disabled, "false"));
    enabled.value = String(user.enabled); accessLabel.append(enabled);
    const save = node("button", text.save);
    if (user.id === getProfile().user.id) { role.disabled = true; enabled.disabled = true; save.disabled = true; }
    form.append(roleLabel, accessLabel, save);
    form.addEventListener("submit", event => {
      event.preventDefault();
      if (user.role === role.value && String(user.enabled) === enabled.value) return;
      run(async () => {
        await api.request("/api/v1/admin/users", { method: "PATCH", body: {
          id: user.id, role: role.value, enabled: enabled.value === "true", version: user.version
        } });
        await load(); notice(text.saved);
      });
    });
    const management = node("td"); management.append(form); row.append(management); return row;
  }
  async function load(requestedPage = page, requestedFilters = filters) {
    const parameters = new URLSearchParams(requestedFilters); parameters.set("page", String(requestedPage));
    const data = await api.request(`/api/v1/admin/users?${parameters}`);
    if (disposed) return;
    filters = new URLSearchParams(requestedFilters); page = data.page; pages = data.pages;
    byId("users").replaceChildren(...data.users.map(userRow));
    byId("users-empty").hidden = data.total !== 0;
    byId("previous-page").hidden = page <= 1;
    byId("next-page").hidden = page >= pages;
    byId("page-summary").textContent = text.pageSummary.replace("{page}", page).replace("{pages}", pages).replace("{total}", data.total);
  }
  on("create-form", "submit", event => {
    event.preventDefault();
    run(async () => {
      const body = Object.fromEntries(new FormData(event.target).entries());
      event.target.elements.password.value = "";
      await api.request("/api/v1/admin/users", { method: "POST", body });
      event.target.reset();
      fillLocales(event.target.elements.locale, getProfile().locales, getProfile().user.locale, language);
      await load(1); notice(text.created);
    });
  });
  on("refresh", "click", () => run(() => load()));
  on("user-filters", "submit", event => {
    event.preventDefault();
    const requested = new URLSearchParams(new FormData(event.target));
    run(() => load(1, requested));
  });
  on("clear-filters", "click", () => run(async () => {
    byId("user-filters").reset();
    await load(1, new URLSearchParams(new FormData(byId("user-filters"))));
  }));
  on("previous-page", "click", () => run(() => load(Math.max(1, page - 1))));
  on("next-page", "click", () => run(() => load(Math.min(pages, page + 1))));
  return { load, dispose() { disposed = true; events.abort(); byId("create-form").reset(); container.replaceChildren(); } };
}
