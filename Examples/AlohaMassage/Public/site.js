"use strict";

// All business data comes from the same endpoint in Drogon and the local preview.
// Query parameters keep navigation usable on an ordinary static document root.
const parameters = new URLSearchParams(window.location.search);
const pages = new Set(["home", "menu", "about", "training", "vouchers", "booking"]);
const page = pages.has(parameters.get("page")) ? parameters.get("page") : "home";
let locale = parameters.get("lang");
let site;
let text;
let backendFeatures = {}, currentAccount = null, accountControls = null;

function element(tag, className, content) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (content !== undefined) node.textContent = content;
  return node;
}

function localised(value) {
  return value?.[locale] ?? value?.[site.defaultLocale] ?? "";
}

function pageUrl(target) {
  const url = new URL("/", window.location.origin);
  url.searchParams.set("page", target);
  url.searchParams.set("lang", locale);
  return url.pathname + url.search;
}

// Content is rendered as text. Links allow only local paths, HTTPS and telephone
// numbers; JSON edits cannot insert scripts, arbitrary markup or data: URLs.
function safeUrl(value) {
  if (typeof value !== "string") throw new Error("Missing content URL");
  const url = new URL(value, window.location.origin);
  if (url.username || url.password) throw new Error("URL credentials are not supported");
  if (url.origin === window.location.origin || url.protocol === "https:" || url.protocol === "tel:") {
    return url.href;
  }
  throw new Error("Unsupported content URL");
}

function link(label, url, className = "button") {
  const node = element("a", className, label);
  node.href = safeUrl(url);
  return node;
}

function assetUrl(value) {
  const url = new URL(value, window.location.origin);
  if (typeof value !== "string" || url.username || url.password ||
      url.origin !== window.location.origin || !url.pathname.startsWith("/assets/")) {
    throw new Error("Images must be local public assets");
  }
  return url.href;
}

function picture(item) {
  const node = element("img");
  node.src = assetUrl(item.src);
  node.alt = localised(item.alt);
  node.loading = "lazy";
  node.width = 600;
  node.height = 450;
  return node;
}

function heading(title, introduction) {
  const group = element("div", "centered");
  const ornament = site.presentation?.images?.ornament;
  if (ornament) {
    const flower = element("img", "flower");
    Object.assign(flower, { src: assetUrl(ornament), alt: "", width: 59, height: 45 });
    group.append(flower);
  }
  group.append(element("h2", "", title));
  if (introduction) group.append(element("p", "intro", introduction));
  return group;
}

function gallery() {
  const group = element("div", "gallery");
  for (const item of site.gallery) {
    const fullImage = link("", item.src, "");
    fullImage.append(picture(item));
    group.append(fullImage);
  }
  return group;
}

function contactSection() {
  const section = element("section", "section wrap centered");
  section.append(heading(text.contactTitle));
  const grid = element("div", "contact-grid");
  const address = element("div");
  address.append(element("h3", "", text.address), element("p", "", localised(site.business.address)),
    link(text.directions, site.business.mapUrl, "text-link"));
  const hours = element("div");
  hours.append(element("h3", "", text.hours), element("p", "", localised(site.business.hours)),
    link(text.book, pageUrl("booking"), "text-link"));
  const contact = element("div");
  contact.append(element("h3", "", text.contact),
    link(site.business.phoneDisplay, `tel:${site.business.phone}`, "text-link"), element("br"),
    link(text.whatsapp, site.business.social.whatsapp, "text-link"));
  grid.append(address, hours, contact);
  section.append(grid);
  return section;
}

function promotions() {
  const group = element("div");
  const now = Date.now();
  for (const offer of site.promotions) {
    // End is exclusive. Dates include their studio timezone offset.
    if (!(Date.parse(offer.startsAt) <= now && now < Date.parse(offer.endsAt))) continue;
    const item = element("aside", "promotion");
    item.append(element("h3", "", localised(offer.title)),
      element("p", "", localised(offer.description)), link(text.ask, offer.url, "text-link"));
    group.append(item);
  }
  return group;
}

function homePage() {
  const group = document.createDocumentFragment();
  const studio = element("section", "section wrap centered");
  studio.append(heading(text.studioTitle, text.studioIntro), gallery(),
    element("p", "intro", text.studioText), link(text.discover, pageUrl("about")));
  const feature = element("section", "section floral-section");
  const split = element("div", "wrap split");
  split.append(picture(site.presentation?.featuredImage ?? site.gallery[0]));
  const copy = element("div");
  copy.append(element("p", "eyebrow", text.professional), element("h2", "", text.menu),
    element("p", "", text.menuIntro));
  const actions = element("div", "actions");
  actions.append(link(text.menu, pageUrl("menu")), link(text.book, pageUrl("booking"), "button button-filled"));
  copy.append(actions);
  split.append(copy);
  feature.append(split);
  const gifts = element("section", "section wrap centered");
  gifts.append(heading(text.voucherTitle, text.voucherIntro),
    element("p", "intro", text.voucherText), link(text.vouchers, pageUrl("vouchers")), promotions());
  const reviews = element("section", "section floral-section centered");
  const reviewsCopy = element("div", "wrap");
  reviewsCopy.append(heading(text.reviewsTitle, text.reviewsText),
    link(text.readReviews, site.business.mapUrl));
  reviews.append(reviewsCopy);
  group.append(studio, feature, gifts, reviews, contactSection());
  return group;
}

function money(variant) {
  return new Intl.NumberFormat(site.presentation?.formatLocales?.[locale] ?? locale, {
    style: "currency", currency: variant.currency,
    maximumFractionDigits: 2, minimumFractionDigits: 0
  }).format(variant.priceMinor / 100);
}

function serviceRow(service) {
  const row = element("details", "service");
  const summary = element("summary");
  const knownPrices = service.variants.filter(v => Number.isSafeInteger(v.priceMinor) && v.priceMinor >= 0);
  const cheapest = knownPrices.reduce((best, v) => !best || v.priceMinor < best.priceMinor ? v : best, null);
  const priceLabel = service.available === false ? text.unavailable :
    cheapest ? `${service.variants.length > 1 ? text.from + " " : ""}${money(cheapest)}` : text.priceOnBooking;
  summary.append(element("span", "service-name", localised(service.title)), element("span", "service-price", priceLabel));
  const detail = element("div", "service-detail");
  detail.append(element("p", "", localised(service.description)));
  const table = element("table", "prices");
  table.setAttribute("aria-label", localised(service.title));
  const tableHead = element("thead");
  const headerRow = element("tr");
  for (const title of [text.duration, text.price]) {
    const th = element("th", "", title);
    th.scope = "col";
    headerRow.append(th);
  }
  tableHead.append(headerRow);
  const tableBody = element("tbody");
  for (const variant of service.variants) {
    const tr = element("tr");
    tr.append(element("td", "", `${variant.durationMinutes} ${text.minutes}`),
      element("td", "", variant.priceMinor === null ? text.priceOnBooking : money(variant)));
    tableBody.append(tr);
  }
  table.append(tableHead, tableBody);
  if (service.variants.length && service.available !== false) detail.append(table);
  if (service.available !== false) {
    detail.append(link(service.bookingUrl.startsWith("tel:") ? text.callToBook : (text.externalBooking ?? text.fresha ?? text.book), service.bookingUrl));
  }
  row.append(summary, detail);
  return row;
}

function menuPage() {
  const section = element("section", "section wrap");
  section.append(heading(text.menu, text.menuIntro), promotions());
  const controls = element("div", "catalog-tools");
  const searchLabel = element("label", "field", text.search);
  const search = element("input");
  search.type = "search";
  search.placeholder = text.searchPlaceholder;
  search.maxLength = 100;
  searchLabel.append(search);
  const categoryLabel = element("label", "field", text.category);
  const category = element("select");
  const all = element("option", "", text.allCategories);
  all.value = "";
  category.append(all);
  for (const item of site.categories) {
    const option = element("option", "", localised(item.title));
    option.value = item.id;
    category.append(option);
  }
  categoryLabel.append(category);
  controls.append(searchLabel, categoryLabel);
  const count = element("p", "catalog-count");
  count.setAttribute("role", "status");
  const list = element("div");
  function filter() {
    const query = search.value.trim().toLocaleLowerCase(locale);
    const matches = site.services.filter(item =>
      (!category.value || item.category === category.value) &&
      `${localised(item.title)} ${localised(item.description)}`.toLocaleLowerCase(locale).includes(query));
    count.textContent = `${text.results} ${matches.length}`;
    list.replaceChildren(...matches.map(serviceRow));
    if (!matches.length) list.append(element("p", "", text.noResults));
  }
  search.addEventListener("input", filter);
  category.addEventListener("change", filter);
  filter();
  const note = element("div", "promotion");
  note.append(element("p", "", text.menuNote), link(text.ask, `tel:${site.business.phone}`, "text-link"));
  section.append(controls, count, list, note);
  return section;
}

function aboutPage() {
  const group = document.createDocumentFragment();
  const section = element("section", "section wrap");
  section.append(heading(text.studioTitle, text.aboutIntro), element("p", "intro centered", text.aboutText), gallery(),
    element("h2", "centered", text.teamTitle));
  const team = element("div", "team centered");
  for (const person of site.team) {
    const profile = element("article");
    profile.append(picture(person.image), element("h3", "", localised(person.name)),
      element("p", "eyebrow", localised(person.role)), element("p", "", localised(person.bio)),
      element("p", "", localised(person.languages)));
    team.append(profile);
  }
  section.append(team);
  group.append(section, contactSection());
  return group;
}

function trainingPage() {
  const group = document.createDocumentFragment();
  const section = element("section", "section wrap reading");
  section.append(heading(text.trainingTitle, text.trainingIntro), element("p", "", text.trainingText));
  for (const course of site.courses) {
    const article = element("article", "course-card");
    const points = element("ul");
    for (const point of course.details) points.append(element("li", "", localised(point)));
    article.append(element("h3", "", localised(course.title)), element("p", "", localised(course.description)),
      element("p", "course-price", course.priceMinor === null ? text.priceOnBooking : money(course)),
      points, link(text.ask, `tel:${site.business.phone}`));
    section.append(article);
  }
  section.append(element("p", "promotion", text.courseNote));
  group.append(section, contactSection());
  return group;
}

function vouchersPage() {
  const group = document.createDocumentFragment();
  const section = element("section", "section wrap split");
  section.append(picture(site.presentation?.voucherImage ?? site.gallery[1]));
  const copy = element("div");
  copy.append(element("p", "eyebrow", text.voucherIntro), element("h2", "", text.voucherTitle),
    element("p", "", text.voucherText), element("p", "", text.voucherNote));
  section.append(copy);
  const options = element("section", "section wrap voucher-options");
  options.setAttribute("aria-label", text.vouchers);
  for (const voucher of site.voucherOptions) {
    const card = element("article", "voucher-card centered");
    card.append(heading(localised(voucher.title)), element("p", "", localised(voucher.description)),
      element("p", "voucher-price", money(voucher)),
      link(text.voucherAction, voucher.purchaseUrl, "button button-filled"));
    options.append(card);
  }
  const details = element("section", "section wrap reading");
  details.append(heading(text.voucherTerms), informationList(site.voucherDetails));
  group.append(section, options, details, contactSection());
  return group;
}

function informationList(items) {
  const list = element("div", "information-list");
  for (const item of items) {
    const article = element("article");
    article.append(element("h3", "", localised(item.title)), element("p", "", localised(item.description)));
    list.append(article);
  }
  return list;
}

function bookingPage() {
  const group = document.createDocumentFragment();
  const section = element("section", "section wrap reading");
  section.append(heading(text.book, text.bookingIntro), element("p", "", text.bookingText));
  const actions = element("div", "actions");
  actions.append(link((text.externalBooking ?? text.fresha ?? text.book), site.business.bookingUrl, "button button-filled"),
    link(text.callToBook, `tel:${site.business.phone}`), link(text.menu, pageUrl("menu"), "text-link"));
  section.append(actions, informationList(site.bookingDetails));
  const native = element("aside", "native-booking"); native.id = "native-booking"; native.hidden = true;
  section.prepend(native);
  group.append(section, contactSection());
  return group;
}

function renderBranding() {
  const images = site.presentation?.images ?? {};
  for (const [property, key] of Object.entries({
    "--hero-image": "hero", "--page-header-image": "pageHeader",
    "--section-image": "sectionBackground", "--footer-image": "footerBackground"
  })) {
    const value = images[key];
    document.documentElement.style.setProperty(property, value ? `url(${JSON.stringify(assetUrl(value))})` : "none");
  }
  const logo = document.querySelector(".brand img");
  logo.hidden = !images.logo;
  if (images.logo) logo.src = assetUrl(images.logo);
  logo.alt = localised(site.business.name);
  const icon = document.querySelector('link[rel="icon"]');
  if (images.logo) icon.href = assetUrl(images.logo);
  else icon.removeAttribute("href");
}

function renderSocialLinks() {
  const social = document.querySelector(".social-links");
  social.replaceChildren();
  for (const item of site.presentation?.socialLinks ?? []) {
    const url = site.business.social?.[item.id];
    if (!url) continue;
    const anchor = link(item.label, url, "");
    if (item.icon) {
      const image = element("img");
      Object.assign(image, { src: assetUrl(item.icon), alt: item.label, width: 24, height: 24 });
      anchor.replaceChildren(image);
    }
    social.append(anchor);
  }
}

function renderLanguages() {
  const languages = document.getElementById("language-switch");
  languages.replaceChildren();
  for (const other of site.locales.filter(value => value !== locale)) {
    const url = new URL(pageUrl(page), window.location.origin);
    url.searchParams.set("lang", other);
    const anchor = link(other.toUpperCase(), url.pathname + url.search + window.location.hash, "language");
    anchor.lang = other;
    anchor.hreflang = other;
    anchor.setAttribute("aria-label", site.presentation?.languageNames?.[other] ?? other);
    languages.append(anchor);
  }
}

function renderNavigation() {
  document.querySelectorAll("[data-i18n]").forEach(node => { node.textContent = text[node.dataset.i18n]; });
  document.querySelectorAll("[data-page]").forEach(node => {
    node.href = pageUrl(node.dataset.page);
    if (node.dataset.page === page && node.closest("nav")) node.setAttribute("aria-current", "page");
    else node.removeAttribute("aria-current");
  });
  document.querySelectorAll("[data-booking]").forEach(node => { node.href = pageUrl("booking"); });
  document.querySelectorAll("[data-phone]").forEach(node => {
    node.href = safeUrl(`tel:${site.business.phone}`);
    node.textContent = site.business.phoneDisplay;
  });
  document.getElementById("navigation").setAttribute("aria-label", text.navigation);
  document.getElementById("retry").textContent = text.retry;
  renderSocialLinks();
  renderLanguages();
}

function renderHero() {
  const home = page === "home";
  document.getElementById("hero").classList.toggle("compact", !home);
  document.getElementById("hero-title").textContent = text.pageTitles[page];
  document.getElementById("hero-subtitle").textContent = text.heroSubtitle;
  document.getElementById("hero-subtitle").hidden = !home;
  document.getElementById("hero-actions").hidden = !home;
  document.getElementById("hero-eyebrow").textContent = text.shortBrand;
  document.getElementById("hero-eyebrow").hidden = home;
}

function render() {
  document.documentElement.lang = locale;
  document.title = `${text.pageTitles[page]} · ${text.shortBrand}`;
  document.querySelector('meta[name="description"]').content = `${text.heroSubtitle} ${localised(site.business.address)}. ${localised(site.business.hours)}.`;
  renderBranding();
  renderNavigation();
  renderHero();
  document.querySelectorAll("[data-site-content]").forEach(node => { node.hidden = false; });
  document.getElementById("loading-message").hidden = true;
  const renderers = {
    home: homePage, menu: menuPage, about: aboutPage, training: trainingPage,
    vouchers: vouchersPage, booking: bookingPage
  };
  document.getElementById("page-content").replaceChildren(renderers[page]());
  document.getElementById("copyright").textContent = `© ${new Date().getFullYear()} ${localised(site.business.name)}`;
  updateNativeBooking();
}

async function load() {
  const error = document.getElementById("load-error");
  const retry = document.getElementById("retry");
  retry.disabled = true;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 10000);
  try {
    const response = await fetch("/api/v1/site", { signal: controller.signal, headers: { Accept: "application/json" } });
    if (!response.ok) throw new Error(`Content request failed: ${response.status}`);
    const content = await response.json();
    if (content.schemaVersion !== 1 || !Array.isArray(content.locales) || !content.locales.includes(content.defaultLocale) ||
      !content.translations?.[content.defaultLocale] || !Array.isArray(content.services)) {
      throw new Error("Unsupported site content");
    }
    site = content;
    locale = site.locales.includes(parameters.get("lang")) ? parameters.get("lang") : site.defaultLocale;
    text = site.translations[locale] ?? site.translations[site.defaultLocale];
    render();
    accountControls?.setLanguage(locale);
    error.hidden = true;
  } catch (failure) {
    console.error("Unable to load site content:", failure);
    document.getElementById("error-text").textContent = text?.error ?? "We could not load the content. Please try again.";
    document.getElementById("loading-message").hidden = true;
    error.hidden = false;
  } finally {
    clearTimeout(timeout);
    retry.disabled = false;
  }
}

document.getElementById("retry").addEventListener("click", load);
const contentReady = load();

// Account links can wrap as the session or language changes, especially on phones.
new ResizeObserver(entries => {
  document.documentElement.style.setProperty("--site-header-height", `${entries[0].target.offsetHeight}px`);
}).observe(document.querySelector(".site-header"));

// Optional backend capabilities are independent of this site's public content.
// Public-only servers and the Python preview keep account controls hidden.
async function startAccounts() {
  try {
    const response = await fetch("/api/v1/features", { headers: { Accept: "application/json" } });
    backendFeatures = response.ok ? await response.json() : {};
    updateNativeBooking();
    if (backendFeatures.accounts !== true) return;
    const { mountPublicAccount } = await import("/accounts-assets/public-account.js");
    // Use the content language after loading; the shared account interface has
    // its own configured fallback for languages it does not yet translate.
    await contentReady;
    accountControls = await mountPublicAccount({
      navigation: document.getElementById("account-navigation"),
      dialog: document.getElementById("login-dialog"),
      profileLink: document.getElementById("account-link"),
      features: backendFeatures, language: locale,
      onChange(user) { currentAccount = user; updateNativeBooking(); }
    });
  } catch { /* Account availability does not block the public site. */ }
}
startAccounts();

function updateNativeBooking() {
  const target = document.getElementById("native-booking");
  if (!target) return;
  target.hidden = !backendFeatures.reservations || (currentAccount !== null && currentAccount.role !== "customer");
  if (target.hidden) return;
  target.replaceChildren(element("h2", "", text.nativeBookingTitle ?? text.book),
    element("p", "", text.nativeBookingText ?? ""),
    link(text.nativeBookingAction ?? text.book, `/profile?lang=${encodeURIComponent(locale)}#section-reservations`, "button button-filled"));
}
