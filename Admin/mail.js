import { messages } from "/accounts-assets/i18n.js";

export function createMail({ container, api, language, run }) {
  const text = messages("mail", language);
  let before = "", next = "", disposed = false, generation = 0;
  const node = (tag, value) => {
    const element = document.createElement(tag);
    if (value !== undefined) element.textContent = value;
    return element;
  };
  const date = value => value ? new Date(value).toLocaleString(language === "bg" ? "bg-BG" : "en-GB") : "—";
  const status = node("p"), totals = node("p"), jobs = node("div"), controls = node("div");
  status.setAttribute("role", "status");
  const filter = node("select"), label = node("label", text.filter);
  for (const value of ["", "queued", "processing", "accepted", "delivered", "delayed", "bounced", "complained", "failed", "skipped"]) {
    const option = node("option", value ? text[value] : text.all); option.value = value; filter.append(option);
  }
  label.append(filter);
  const refresh = node("button", text.refresh), older = node("button", text.older);
  refresh.type = older.type = "button";
  refresh.addEventListener("click", () => run(async () => { before = ""; await load(); }));
  filter.addEventListener("change", () => run(async () => { before = ""; await load(); }));
  older.addEventListener("click", () => run(async () => { before = next; await load(); }));
  controls.append(label, refresh, older);
  container.append(node("h2", text.title), node("p", text.help), controls, status, totals, jobs);
  async function load() {
    const current = ++generation;
    const query = new URLSearchParams();
    if (before) query.set("before", before);
    if (filter.value) query.set("state", filter.value);
    const data = await api.request("/api/v1/admin/mail" + (query.size ? "?" + query : ""));
    if (disposed || current !== generation) return;
    next = data.nextBefore;
    older.disabled = !next;
    const worker = data.worker;
    status.textContent = `${text[data.transport]} · ${worker.healthy ? text.healthy : text.stale} · ${text.checked}: ${date(worker.heartbeatAt)}`;
    if (worker.paused) status.textContent += ` · ${text.paused}: ${date(worker.pauseUntil)}`;
    if (worker.error) status.textContent += ` · ${text[worker.error] || text.unknownError}`;
    totals.textContent = Object.entries(data.counts).map(([state, count]) => `${text[state] || state}: ${count}`).join(" · ");
    jobs.replaceChildren(...data.jobs.map(job => {
      const card = node("article"); card.className = "user-card";
      const state = job.transport === "local_outbox" && job.state === "delivered" ? "outbox" : job.state;
      card.append(node("h3", job.subject), node("p", `${job.recipient} · ${text[job.kind]} · ${text[state]}`),
        node("p", `${text.reference}: ${job.id} · ${text.attempts}: ${job.attempts} · ${date(job.createdAt)}`));
      if (job.error) card.append(node("p", `${text[job.error] || text.unknownError}${job.statusCode ? ` (HTTP ${job.statusCode})` : ""}`));
      if (["queued", "processing"].includes(job.state)) card.append(node("p", `${text.nextAttempt}: ${date(job.availableAt)}`));
      return card;
    }));
    if (!data.jobs.length) jobs.append(node("p", text.empty));
  }
  return { load, dispose() { disposed = true; generation++; container.replaceChildren(); } };
}
