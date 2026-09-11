// This module edits the service schema, independently of account/profile forms.
// Plain text goes through textContent and input values; no content becomes HTML.
import { messages } from "/accounts-assets/i18n.js";
// Parse decimal text into integer minor units, without floating-point rounding.
export function priceToMinor(value) {
  const text = value.trim().replace(",", ".");
  if (!text) return null;
  if (!/^\d{1,7}(\.\d{1,2})?$/.test(text)) throw new RangeError("Invalid price");
  const [whole, fraction = ""] = text.split(".");
  const minor = Number(whole) * 100 + Number(fraction.padEnd(2, "0"));
  if (minor > 100000000) throw new RangeError("Invalid price");
  return minor;
}

export function createServiceEditor({ container, api, language, run, notice }) {
  const text = messages("services", language);
  const endpoint = "/api/v1/admin/content/services";
  let data, selected = "", dirty = false, disposed = false, pending = false;
  let form, fields, selector, save;
  let durationRows = [];
  const status = element("p", text.clean, "muted");

  function element(tag, content, className) {
    const node = document.createElement(tag);
    if (content !== undefined) node.textContent = content;
    if (className) node.className = className;
    return node;
  }
  function button(label, action) {
    const result = element("button", label);
    result.type = "button";
    result.addEventListener("click", action);
    return result;
  }
  function field(parent, label, tag = "input", properties = {}) {
    const wrapper = element("label", label);
    const input = Object.assign(element(tag), properties);
    wrapper.append(input);
    parent.append(wrapper);
    return input;
  }
  function markDirty() {
    dirty = true;
    status.textContent = text.dirty;
  }
  function canDiscard() { return !pending && (!dirty || window.confirm(text.discard)); }
  function beforeUnload(event) {
    if (dirty) { event.preventDefault(); event.returnValue = ""; }
  }
  window.addEventListener("beforeunload", beforeUnload);
  function localized(value) { return value[language] || value[data.defaultLocale]; }

  function addDuration(parent, variant, currency) {
    if (durationRows.length >= 20) { notice(text.maximum, true); return; }
    const row = element("div", undefined, "duration-row");
    const minutes = field(row, text.minutes, "input", { type: "number", min: "1", max: "1440", step: "1", required: true, value: variant?.durationMinutes ?? 60 });
    const minor = variant?.priceMinor;
    const amount = minor == null ? "" : `${Math.floor(minor / 100)}.${String(minor % 100).padStart(2, "0")}`;
    const price = field(row, text.price, "input", { type: "text", inputMode: "decimal", maxLength: 10, value: amount });
    const entry = { row, minutes, price };
    row.append(button(text.remove, () => { row.remove(); durationRows = durationRows.filter(item => item !== entry); markDirty(); }));
    durationRows.push(entry);
    parent.append(row);
    // One currency selector applies to all of the service's duration options.
    if (variant) currency.value = variant.currency;
  }

  function render(service) {
    selected = service?.id || "";
    dirty = !service;
    status.textContent = dirty ? text.dirty : text.clean;
    durationRows = [];
    const controls = element("div", undefined, "editor-toolbar");
    selector = field(controls, text.choose, "select");
    if (!selected) selector.add(new Option(text.newService, ""));
    for (const item of data.services) selector.add(new Option(localized(item.title), item.id));
    selector.value = selected;
    selector.addEventListener("change", () => {
      const id = selector.value;
      if (canDiscard()) render(data.services.find(item => item.id === id));
      else selector.value = selected;
    });
    controls.append(button(text.add, () => { if (canDiscard()) render(null); }));
    controls.append(button(text.reload, () => { if (canDiscard()) run(load); }));
    form = element("form");
    form.method = "post";
    form.className = "content-form";
    fields = element("fieldset", undefined, "editor-fields");
    fields.append(element("legend", service ? localized(service.title) : text.newService));
    fields.append(element("p", `${text.serviceId}: ${selected || text.assigned}`, "muted"));
    const localizedFields = [];
    for (const locale of data.locales) {
      const group = element("fieldset", undefined, "translation-fields");
      group.append(element("legend", locale.toUpperCase()));
      const title = field(group, text.title, "input", { type: "text", required: true, maxLength: 200, value: service?.title[locale] || "", lang: locale });
      const description = field(group, text.description, "textarea", { required: true, maxLength: 4000, rows: 5, value: service?.description[locale] || "", lang: locale });
      localizedFields.push({ locale, title, description });
      fields.append(group);
    }
    const category = field(fields, text.category, "select", { required: true });
    for (const item of data.categories) category.add(new Option(localized(item.title), item.id));
    category.value = service?.category || data.categories[0].id;
    const booking = field(fields, text.booking, "input", { type: "text", required: true, maxLength: 2048, value: service?.bookingUrl || data.defaultBookingUrl });
    const available = field(fields, text.available, "input", { type: "checkbox", checked: service?.available !== false });
    available.parentElement.className = "checkbox-field";
    fields.append(element("p", text.availabilityHelp, "muted"));
    const options = element("fieldset", undefined, "translation-fields");
    options.append(element("legend", text.options));
    const currency = field(options, text.currency, "select");
    for (const code of data.currencies) currency.add(new Option(code, code));
    const rows = element("div");
    for (const variant of service?.variants || []) addDuration(rows, variant, currency);
    options.append(rows, button(text.addOption, () => { addDuration(rows, null, currency); markDirty(); }), element("p", text.priceHelp, "muted"));
    fields.append(options);
    save = element("button", text.save);
    save.type = "submit";
    fields.append(save);
    form.append(fields);
    form.addEventListener("input", markDirty);
    form.addEventListener("change", markDirty);
    form.addEventListener("submit", event => {
      event.preventDefault();
      if (pending) return;
      const edited = { id: selected, title: {}, description: {}, category: category.value,
        available: available.checked, bookingUrl: booking.value.trim(), variants: [] };
      for (const entry of localizedFields) {
        edited.title[entry.locale] = entry.title.value.trim();
        edited.description[entry.locale] = entry.description.value.trim();
      }
      try {
        edited.variants = durationRows.map(entry => ({ durationMinutes: Number(entry.minutes.value), priceMinor: priceToMinor(entry.price.value), currency: currency.value }));
      } catch { notice(text.priceError, true); return; }
      run(async () => {
        pending = true;
        save.textContent = text.publishing;
        status.textContent = text.publishing;
        fields.disabled = true;
        selector.disabled = true;
        try {
          const result = await api.request(endpoint, { method: selected ? "PATCH" : "POST", body: { revision: data.revision, service: edited }, timeoutMs: 60000 });
          if (disposed) return;
          data = result;
          render(data.services.find(item => item.id === result.serviceId));
          notice(text.published);
        } catch (error) {
          if (error.status === 409) error.message = text.conflict;
          throw error;
        } finally {
          pending = false;
          fields.disabled = false;
          selector.disabled = false;
          save.textContent = text.save;
          status.textContent = dirty ? text.dirty : text.clean;
        }
      });
    });
    container.replaceChildren(element("p", text.help, "muted"), controls, status, form);
  }

  async function load() {
    pending = true;
    if (fields) fields.disabled = true;
    if (selector) selector.disabled = true;
    try {
      const result = await api.request(endpoint, { timeoutMs: 60000 });
      if (disposed) return;
      data = result;
      render(data.services.find(item => item.id === selected) || data.services[0]);
    } finally {
      pending = false;
      if (fields) fields.disabled = false;
      if (selector) selector.disabled = false;
    }
  }
  function dispose() {
    disposed = true;
    dirty = false;
    window.removeEventListener("beforeunload", beforeUnload);
    container.replaceChildren();
  }
  return { load, dispose };
}
