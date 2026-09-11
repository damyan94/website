import { messages, fillLocales } from "/accounts-assets/i18n.js";

export function createNewsletters({ container, api, language, locales, run, notice, options }) {
  const text = messages("newsletters", language);
  const delivery = messages("mail", language);
  let draft = null, requestKey = crypto.randomUUID(), disposed = false;
  const node = (tag, value) => { const element = document.createElement(tag); if (value !== undefined) element.textContent = value; return element; };
  const form = node("form"); form.method = "post";
  const fields = node("fieldset"); fields.className = "editor-fields";
  function field(label, tag, properties) { const wrapper = node("label", label), control = Object.assign(node(tag), properties); wrapper.append(control); fields.append(wrapper); return control; }
  const locale = field(text.locale, "select", {});
  fillLocales(locale, locales, language, language);
  const subject = field(text.subject, "input", { required: true, maxLength: 200 });
  const body = field(text.body, "textarea", { required: true, maxLength: 5000, rows: 8 });
  fields.append(node("button", text.preview)); form.append(fields);
  const preview = node("div"), history = node("div");
  const refresh = node("button", text.refresh); refresh.type = "button";
  refresh.addEventListener("click", () => run(load));
  container.replaceChildren(node("h2", text.title), node("p", text.help), node("p", options.transport === "local_outbox" ? text.local : delivery.help), form, preview, node("h3", text.history), refresh, history);
  form.addEventListener("input", () => { draft = null; requestKey = crypto.randomUUID(); preview.replaceChildren(); });
  form.addEventListener("submit", event => {
    event.preventDefault();
    run(async () => {
      fields.disabled = true;
      try {
        const result = await api.request("/api/v1/admin/newsletters/preview", { method: "POST", body: { requestKey, locale: locale.value, subject: subject.value.trim(), text: body.value.trim() } });
        if (disposed) return;
        draft = result;
        const message = node("pre", result.text); message.className = "mail-preview";
        const send = node("button", text.send); send.type = "button";
        send.addEventListener("click", () => {
          if (!draft || !window.confirm(text.confirm)) return;
          run(async () => {
            fields.disabled = true;
            try {
              await api.request("/api/v1/admin/newsletters/send", { method: "POST", body: { id: draft.id, expectedRecipients: draft.recipients }, timeoutMs: 60000 });
              draft = null; preview.replaceChildren(); requestKey = crypto.randomUUID();
              await load(); notice(text.queued);
            } finally { fields.disabled = false; }
          });
        });
        preview.replaceChildren(node("p", `${text.audience}: ${result.recipients}`), node("h3", result.subject), message);
        if (result.recipients > 0 && result.recipients <= 1000) preview.append(send);
      } finally { fields.disabled = false; }
    });
  });
  async function load() {
    const data = await api.request("/api/v1/admin/newsletters");
    if (disposed) return;
    history.replaceChildren(...data.campaigns.map(campaign => {
      const article = node("article"); article.className = "user-card";
      article.append(node("h4", campaign.subject), node("p", `${campaign.locale.toUpperCase()} · ${campaign.state} · ${text.total}: ${campaign.recipients} · ${delivery.outbox}: ${campaign.outbox} · ${delivery.accepted}: ${campaign.accepted} · ${delivery.delivered}: ${Number(campaign.delivered) - Number(campaign.outbox)} · ${text.failed}: ${campaign.failed} · ${text.skipped}: ${campaign.skipped}`));
      return article;
    }));
  }
  return { load, dispose() { disposed = true; container.replaceChildren(); } };
}
