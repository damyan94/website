import { fillLocales, messages } from "/accounts-assets/i18n.js";

export function createReservations({ container, api, language, getProfile, isAdminPage, run, notice }) {
  const text = messages("reservations", language);
  const staff = isAdminPage && ["admin", "operator"].includes(getProfile().user.role);
  const events = new AbortController();
  let options, format, editing = null, requestKey = crypto.randomUUID(), slotRevision = 0, page = 1, pages = 1, disposed = false;
  const node = (tag, value, className) => { const n = document.createElement(tag); if (value !== undefined) n.textContent = value; if (className) n.className = className; return n; };
  const on = (element, event, fn) => element.addEventListener(event, fn, { signal: events.signal });
  const label = (title, control) => { const n = node("label", title); n.append(control); return n; };
  const input = (type, required = false) => { const n = node("input"); n.type = type; n.required = required; return n; };
  const button = (title, fn) => { const n = node("button", title); n.type = "button"; on(n, "click", fn); return n; };
  const local = value => value?.[language] ?? value?.[getProfile().locales[0]] ?? value?.en ?? Object.values(value ?? {})[0] ?? "";
  const money = variant => new Intl.NumberFormat(language, { style: "currency", currency: variant.currency }).format(variant.priceMinor / 100);
  const bookingTitle = staff ? text.newGuestBooking : text.newBooking;
  const formSection = node("section"), details = node("details"), summary = node("summary", bookingTitle), form = node("form"); form.method = "post";
  const policy = node("p", "", "muted"), contact = node("p", "", "muted");
  const profileLink = node("a", text.profile); profileLink.href = "#section-profile";
  const service = node("select"), variant = node("select"), therapist = node("select"), date = input("date", true), slot = node("select");
  const guest = node("fieldset", undefined, "editor-fields"), guestName = input("text", staff), guestEmail = input("email"), guestPhone = input("tel"), guestLocale = node("select");
  guestName.maxLength = 100; guestEmail.maxLength = 254; guestPhone.maxLength = 32;
  guest.append(node("p", text.guestHelp, "muted"), label(text.name, guestName), label(text.email, guestEmail), label(text.phone, guestPhone), label(text.locale, guestLocale)); guest.hidden = !staff;
  slot.required = true;
  const slotsHelp = node("p", "", "muted"), confirm = node("button", text.confirm); confirm.type = "submit";
  const find = button(text.find, () => run(findSlots));
  const stop = button(text.stopEditing, () => { editing = null; setupForm(); }); stop.hidden = true;
  form.append(contact, profileLink, label(text.service, service), label(text.duration, variant), label(text.therapist, therapist), label(text.date, date), find,
    label(text.time, slot), slotsHelp, guest, confirm, stop);
  details.append(summary, form); formSection.append(policy, button(text.reloadOptions, () => run(async () => { editing = null; await load(); })), details); container.append(formSection);
  const listSection = node("section"), heading = node("h2", staff ? text.calendar : text.mine), toolbar = node("div", undefined, "editor-toolbar");
  const calendarDate = input("date", true), days = node("select"), scope = node("select");
  days.add(new Option(text.day, "1")); days.add(new Option(text.week, "7"));
  scope.add(new Option(text.upcoming, "upcoming")); scope.add(new Option(text.history, "history"));
  if (staff) toolbar.append(label(text.date, calendarDate), label(text.view, days)); else toolbar.append(label(text.scope, scope));
  toolbar.append(button(text.refresh, () => run(() => loadList(1))));
  const list = node("div"), paging = node("nav", undefined, "pagination"), pageText = node("span");
  const previous = button(text.previous, () => run(() => loadList(page - 1))), next = button(text.next, () => run(() => loadList(page + 1)));
  paging.append(previous, pageText, next); listSection.append(heading, toolbar, list, paging); container.append(listSection);
  function clearSlots() { slotRevision++; slot.replaceChildren(new Option(text.chooseTime, "")); confirm.hidden = true; slotsHelp.textContent = ""; requestKey = crypto.randomUUID(); }
  function selectedService() { return options.services.find(item => item.id === service.value); }
  function variants() {
    variant.replaceChildren(); therapist.replaceChildren(new Option(text.any, ""));
    const selected = selectedService();
    for (const v of selected?.variants ?? []) variant.add(new Option(`${v.durationMinutes} ${text.minutes} · ${money(v)}`, String(v.durationMinutes)));
    for (const id of selected?.therapists ?? []) therapist.add(new Option(local(options.resources.find(r => r.id === id)?.name), id));
    clearSlots();
  }
  function setupForm() {
    summary.textContent = bookingTitle; service.disabled = false; variant.disabled = false; guest.hidden = !staff; guest.disabled = !staff;
    stop.hidden = true; confirm.textContent = text.confirm; clearSlots();
  }
  async function findSlots() {
    clearSlots();
    if (!date.reportValidity() || !selectedService()) return;
    const revision = slotRevision;
    const params = new URLSearchParams({ serviceId: service.value, durationMinutes: variant.value, revision: options.revision, date: date.value, therapistId: therapist.value });
    if (editing) params.set("excludeId", editing.id);
    const data = await api.request(`/api/v1/reservations/availability?${params}`);
    if (disposed || revision !== slotRevision) return;
    for (const item of data.slots) slot.add(new Option(format.format(new Date(item.startsAt)), item.startsAt));
    slotsHelp.textContent = data.slots.length ? "" : text.noSlots; confirm.hidden = data.slots.length === 0;
  }
  for (const control of [variant, therapist, date]) on(control, "change", clearSlots);
  on(service, "change", variants);
  on(guest, "input", () => { requestKey = crypto.randomUUID(); });
  function edit(appointment) {
    editing = appointment; details.open = true; summary.textContent = text.rescheduling;
    service.value = appointment.serviceId; variants(); variant.value = String(appointment.durationMinutes); therapist.value = appointment.therapistId;
    service.disabled = true; variant.disabled = true; guest.hidden = true; guest.disabled = true;
    date.value = dateKey(appointment.startsAt); confirm.textContent = text.reschedule; stop.hidden = false;
    summary.scrollIntoView({ block: "start" });
  }
  on(form, "submit", event => {
    event.preventDefault();
    if (!slot.value) return;
    const rescheduling = Boolean(editing);
    const body = editing ? { id: editing.id, version: editing.version, action: "reschedule", revision: options.revision, date: date.value, startsAt: slot.value, therapistId: therapist.value } :
      { serviceId: service.value, durationMinutes: Number(variant.value), revision: options.revision, date: date.value, startsAt: slot.value, therapistId: therapist.value, requestKey };
    if (!editing && staff) body.guest = { name: guestName.value, email: guestEmail.value, phone: guestPhone.value, locale: guestLocale.value };
    run(async () => {
      await api.request("/api/v1/reservations", { method: rescheduling ? "PATCH" : "POST", body });
      if (disposed) return;
      const message = rescheduling ? text.saved : text.confirmed;
      editing = null; setupForm(); if (staff) calendarDate.value = body.date; else scope.value = "upcoming";
      guestName.value = ""; guestEmail.value = ""; guestPhone.value = "";
      await loadList(1); notice(message);
    });
  });
  function dateKey(instant) {
    const parts = new Intl.DateTimeFormat("en", { timeZone: options.timezone, year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(new Date(instant));
    const value = kind => parts.find(p => p.type === kind).value;
    return `${value("year")}-${value("month")}-${value("day")}`;
  }
  function card(appointment) {
    const card = node("article", undefined, "appointment");
    card.append(node("h3", local(appointment.serviceTitle)), node("p", format.format(new Date(appointment.startsAt))),
      node("p", `${appointment.durationMinutes} ${text.minutes} · ${money(appointment)}`),
      node("p", appointment.state === "confirmed" ? text.booked : text[appointment.state], "appointment-state"));
    if (staff) card.append(node("p", `${appointment.contactName} · ${appointment.contactEmail} · ${appointment.contactPhone}`));
    const resource = id => local(options.resources.find(r => r.id === id)?.name) || id;
    card.append(node("p", `${text.therapist}: ${resource(appointment.therapistId)} · ${text.room}: ${resource(appointment.roomId)}`, "muted"));
    const actions = node("div", undefined, "appointment-actions");
    const update = action => run(async () => { await api.request("/api/v1/reservations", { method: "PATCH", body: { id: appointment.id, version: appointment.version, action } }); await loadList(); notice(text.saved); });
    if (appointment.state === "confirmed") {
      if (staff || appointment.canCancel) actions.append(button(text.cancel, () => { if (window.confirm(`${text.cancelQuestion}\n${local(appointment.serviceTitle)} · ${format.format(new Date(appointment.startsAt))}`)) update("cancelled"); }));
      if (staff) {
        if (options.services.some(s => s.id === appointment.serviceId && s.variants.some(v => v.durationMinutes === appointment.durationMinutes))) actions.append(button(text.reschedule, () => edit(appointment)));
        if (Date.parse(appointment.endsAt) <= Date.now()) actions.append(button(text.complete, () => update("completed")));
        if (Date.parse(appointment.startsAt) <= Date.now()) actions.append(button(text.noShow, () => update("no_show")));
      }
    }
    card.append(actions); return card;
  }
  async function loadList(requested = page) {
    const params = new URLSearchParams({ page: String(Math.max(1, requested)) });
    if (staff) { if (!calendarDate.reportValidity()) return; params.set("date", calendarDate.value); params.set("days", days.value); } else params.set("scope", scope.value);
    const data = await api.request(`${staff ? "/api/v1/admin/reservations" : "/api/v1/me/reservations"}?${params}`);
    if (disposed) return;
    page = data.page; pages = data.pages; list.replaceChildren(); list.className = staff ? "booking-calendar" : "";
    if (!data.appointments.length) list.append(node("p", text.empty));
    const groups = new Map();
    for (const appointment of data.appointments) {
      if (!staff) { list.append(card(appointment)); continue; }
      const day = dateKey(appointment.startsAt);
      if (!groups.has(day)) { const group = node("div"); group.append(node("h3", day)); groups.set(day, group); list.append(group); }
      groups.get(day).append(card(appointment));
    }
    previous.hidden = page <= 1; next.hidden = page >= pages;
    pageText.textContent = text.page.replace("{page}", page).replace("{pages}", pages).replace("{total}", data.total);
  }
  async function load() {
    options = await api.request("/api/v1/reservations/options");
    if (disposed) return;
    format = new Intl.DateTimeFormat(language, { timeZone: options.timezone, year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", timeZoneName: "short" });
    policy.textContent = text.policy.replace("{zone}", options.timezone).replace("{hours}", options.cancelHours);
    service.replaceChildren();
    for (const item of options.services) service.add(new Option(local(item.title), item.id));
    const requested = new URLSearchParams(location.search).get("service"); if (options.services.some(s => s.id === requested)) service.value = requested;
    date.value = options.today; date.min = options.today; date.max = options.maxDate; calendarDate.value = options.today;
    fillLocales(guestLocale, getProfile().locales, getProfile().user.locale, language);
    variants(); setupForm(); update(getProfile());
    form.hidden = options.services.length === 0; if (form.hidden) summary.textContent = text.unavailable;
    await loadList();
  }
  function update(profile) {
    contact.textContent = staff ? "" : profile.user.emailVerified ? `${text.contact}: ${profile.user.displayName} · ${profile.user.email} · ${profile.user.phone}` : text.verifiedHelp;
    profileLink.hidden = staff || profile.user.emailVerified; find.disabled = !staff && !profile.user.emailVerified;
  }
  return { load, update, dispose() { disposed = true; events.abort(); container.replaceChildren(); } };
}
