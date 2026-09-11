// Shared interface copy. Website content languages remain independent.
export const interfaceLocales = Object.freeze(["en", "bg"]);

const dictionaries = {
  accounts: {
  en: {
    reservations: "Reservations", close: "Close",
    website: "Back to website", profile: "My profile", administration: "Administration", logout: "Sign out",
    account: "Your account", login: "Sign in", email: "Email", password: "Password", contactAdmin: "For account help, contact support.",
    name: "Name", phone: "Phone (optional)", preferredLanguage: "Preferred language", save: "Save changes", changePassword: "Change password",
    currentPassword: "Current password", newPassword: "New password", passwordHelp: "Use 15–128 characters. A long, unique passphrase works well. Changing your password signs out all devices.",
    createAccount: "Create an account", role: "Role", customer: "Customer", operator: "Operator", admin: "Administrator", initialPassword: "Initial password",
    initialPasswordHelp: "Use a unique password and share it privately with the account owner. They can change it after signing in.", users: "Users", refresh: "Refresh",
    searchUsers: "Search name or email", allRoles: "All roles", allAccess: "All accounts", sortBy: "Sort by", sortOrder: "Order",
    createdAt: "Created", lastLoginAt: "Last login", noRecordedLogin: "No recorded login", ascending: "Ascending / oldest first", descending: "Descending / newest first",
    applyFilters: "Apply filters", clearFilters: "Clear filters", noUsers: "No accounts match these filters.", management: "Management",
    emailVerification: "Email verification", emailConfirmed: "Verified", emailUnconfirmed: "Not verified",
    previousPage: "Previous", nextPage: "Next", pagination: "User pages", pageSummary: "Page {page} of {pages} · Accounts: {total}",
    userDatesHelp: "Dates use your device's time zone. Last login records successful sign-ins, not current online status.",
    accessHelp: "Changing access signs that account out on every device. Your own administrator account cannot be disabled or demoted here.", more: "Load more",
    enabled: "Enabled", disabled: "Disabled", access: "Access", saved: "Changes saved.", created: "Account created.", signedOut: "Signed out.", signedInAs: "Signed in as",
    passwordChanged: "Password changed. Sign in again with your new password.", denied: "This account does not have administrator access.", expired: "Please sign in again.",
    connectionError: "The request could not be completed. Reload to check the current state before retrying.",
    register: "Register", recover: "Forgot password?", emailSettings: "Email settings", verifyEmail: "Send verification link", verified: "Email verified.", unverified: "Verify your email to enable password recovery and newsletters.",
    newEmail: "New email address", changeEmail: "Request email change", emailChangeHelp: "Confirm the new address by email. This signs out all devices and removes the old newsletter subscription.",
    mail: "Email delivery", newsletters: "Newsletters and offers", subscribed: "You are subscribed.", unsubscribed: "You are not subscribed.", subscribe: "Subscribe by email", unsubscribe: "Unsubscribe", subscriptionHelp: "Optional promotional email. You can unsubscribe at any time; account messages are separate.", emailRequested: "If eligible, a confirmation link will arrive shortly. Please allow a minute before requesting another link.",
    interfaceLanguage: "Interface language", sections: "Account sections", retry: "Retry", serviceMenu: "Service menu", bg: "Bulgarian", en: "English"
  },
  bg: {
    reservations: "Резервации", close: "Затворете",
    website: "Към сайта", profile: "Моят профил", administration: "Администрация", logout: "Изход",
    account: "Вашият профил", login: "Вход", email: "Имейл", password: "Парола", contactAdmin: "За помощ с профила се свържете с поддръжката.",
    name: "Име", phone: "Телефон (незадължителен)", preferredLanguage: "Предпочитан език", save: "Запазете промените", changePassword: "Смяна на парола",
    currentPassword: "Текуща парола", newPassword: "Нова парола", passwordHelp: "Използвайте 15–128 знака. Дълга, уникална фраза е добър избор. Смяната на паролата прекратява входа на всички устройства.",
    createAccount: "Създаване на профил", role: "Роля", customer: "Клиент", operator: "Оператор", admin: "Администратор", initialPassword: "Начална парола",
    initialPasswordHelp: "Използвайте уникална парола и я предайте лично на собственика. Той може да я смени след вход.", users: "Потребители", refresh: "Обновете",
    searchUsers: "Търсене по име или имейл", allRoles: "Всички роли", allAccess: "Всички профили", sortBy: "Подреждане по", sortOrder: "Ред",
    createdAt: "Създаден на", lastLoginAt: "Последен вход", noRecordedLogin: "Няма записан вход", ascending: "Възходящ / най-старите първо", descending: "Низходящ / най-новите първо",
    applyFilters: "Приложете филтрите", clearFilters: "Изчистете филтрите", noUsers: "Няма профили с тези филтри.", management: "Управление",
    emailVerification: "Потвърждение на имейл", emailConfirmed: "Потвърден", emailUnconfirmed: "Непотвърден",
    previousPage: "Предишна", nextPage: "Следваща", pagination: "Страници с потребители", pageSummary: "Страница {page} от {pages} · Профили: {total}",
    userDatesHelp: "Датите са в часовата зона на устройството Ви. Последният вход показва успешно влизане, а не текущо присъствие онлайн.",
    accessHelp: "Промяната на достъпа прекратява входа на всички устройства. Не можете да изключите своя администраторски профил или да понижите ролята му тук.", more: "Заредете още",
    enabled: "Разрешен", disabled: "Забранен", access: "Достъп", saved: "Промените са запазени.", created: "Профилът е създаден.", signedOut: "Излязохте от профила.", signedInAs: "Влезли сте като",
    passwordChanged: "Паролата е сменена. Влезте отново с новата парола.", denied: "Този профил няма администраторски достъп.", expired: "Моля, влезте отново.",
    connectionError: "Заявката не може да бъде завършена. Презаредете, за да проверите състоянието, преди да опитате пак.",
    register: "Регистрация", recover: "Забравена парола?", emailSettings: "Настройки за имейл", verifyEmail: "Изпратете връзка за потвърждение", verified: "Имейлът е потвърден.", unverified: "Потвърдете имейла за възстановяване на парола и абонамент за бюлетини.",
    newEmail: "Нов имейл адрес", changeEmail: "Заявете промяна на имейл", emailChangeHelp: "Потвърдете новия адрес по имейл. Това прекратява входа на всички устройства и премахва стария абонамент за бюлетина.",
    mail: "Изпращане на имейли", newsletters: "Бюлетини и предложения", subscribed: "Абонирани сте.", unsubscribed: "Не сте абонирани.", subscribe: "Абонирайте се по имейл", unsubscribe: "Отписване", subscriptionHelp: "Промоционалните имейли са по желание. Можете да се отпишете по всяко време; съобщенията за профила са отделни.", emailRequested: "Ако заявката е допустима, скоро ще получите връзка за потвърждение. Изчакайте поне минута преди нова заявка.",
    interfaceLanguage: "Език на интерфейса", sections: "Раздели на профила", retry: "Опитайте отново", serviceMenu: "Меню с услуги", bg: "Български", en: "Английски"
  }
},
  reservations: {
    en: {
      reloadOptions: "Reload booking options",
      newBooking: "Book an appointment", newGuestBooking: "Book for a guest", calendar: "Reservation calendar", mine: "My reservations", service: "Service", duration: "Duration and price", therapist: "Therapist", any: "Any available therapist",
      date: "Date", find: "Find available times", time: "Available time", chooseTime: "Choose a time", noSlots: "No available times for this selection.", confirm: "Confirm reservation", confirmed: "Reservation confirmed.",
      name: "Guest name", email: "Guest email (optional)", phone: "Guest phone (optional)", locale: "Guest language", guestHelp: "Staff booking for a guest. Provide email or phone. This does not create or link a login account.",
      verifiedHelp: "Verify your email in My profile before booking.", contact: "Booking contact", profile: "My profile", refresh: "Refresh", upcoming: "Upcoming", history: "Past and cancelled", scope: "Show", day: "Day", week: "Week", view: "Calendar view", empty: "No reservations in this view.",
      cancel: "Cancel reservation", cancelQuestion: "Cancel this reservation?", cancelled: "Cancelled", completed: "Completed", no_show: "No-show", complete: "Mark completed", noShow: "Mark no-show", reschedule: "Reschedule", rescheduling: "Reschedule appointment", stopEditing: "Stop rescheduling", saved: "Reservation updated.",
      previous: "Previous", next: "Next", page: "Page {page} of {pages} · Appointments: {total}", policy: "Studio time zone: {zone}. Customer cancellation: at least {hours} hours before the appointment.", booked: "Confirmed", room: "Room", minutes: "min", loading: "Loading…", unavailable: "No services are configured for native booking.", useProfile: "Guests should contact staff to change their reservation."
    },
    bg: {
      reloadOptions: "Презаредете опциите за резервация",
      newBooking: "Запазете час", newGuestBooking: "Запишете гост", calendar: "Календар с резервации", mine: "Моите резервации", service: "Услуга", duration: "Продължителност и цена", therapist: "Терапевт", any: "Всеки свободен терапевт",
      date: "Дата", find: "Покажете свободните часове", time: "Свободен час", chooseTime: "Изберете час", noSlots: "Няма свободни часове за този избор.", confirm: "Потвърдете резервацията", confirmed: "Резервацията е потвърдена.",
      name: "Име на госта", email: "Имейл на госта (незадължителен)", phone: "Телефон на госта (незадължителен)", locale: "Език на госта", guestHelp: "Резервация от служител за гост. Въведете имейл или телефон. Това не създава и не свързва потребителски профил.",
      verifiedHelp: "Потвърдете имейла си в „Моят профил“, преди да резервирате.", contact: "Данни за резервацията", profile: "Моят профил", refresh: "Обновете", upcoming: "Предстоящи", history: "Минали и отказани", scope: "Покажете", day: "Ден", week: "Седмица", view: "Изглед на календара", empty: "Няма резервации в този изглед.",
      cancel: "Откажете резервацията", cancelQuestion: "Да се откаже ли тази резервация?", cancelled: "Отказана", completed: "Завършена", no_show: "Неявяване", complete: "Отбележете като завършена", noShow: "Отбележете неявяване", reschedule: "Преместете", rescheduling: "Преместване на резервация", stopEditing: "Спрете преместването", saved: "Резервацията е обновена.",
      previous: "Предишна", next: "Следваща", page: "Страница {page} от {pages} · Резервации: {total}", policy: "Часова зона на студиото: {zone}. Отказ от клиент: поне {hours} часа преди часа.", booked: "Потвърдена", room: "Стая", minutes: "мин", loading: "Зареждане…", unavailable: "Няма настроени услуги за вътрешни резервации.", useProfile: "Гостите трябва да се свържат със служител за промяна на резервация."
    }
  },
  services: {
  en: {
    help: "Edit a service below. Saving publishes it immediately; visitors see the change when they reload the website.",
    choose: "Service", add: "Add service", reload: "Reload menu", title: "Name", description: "Description",
    category: "Category", available: "Available for booking", booking: "Booking link (https://… or tel:+…)",
    options: "Durations and prices", minutes: "Minutes", price: "Price", currency: "Currency", remove: "Remove option",
    addOption: "Add duration", priceHelp: "Leave a price blank for a quote when booking. Use a decimal point or comma, with up to two decimal places. Remove all duration options for a consultation-only service.",
    availabilityHelp: "Unavailable services remain visible, with booking disabled. Existing reservations are unaffected.",
    save: "Save and publish", publishing: "Publishing…", published: "Service published. Reload the public website to see it.",
    dirty: "Unpublished changes", clean: "Published content", newService: "New service",
    discard: "Discard the unpublished changes to this service?", priceError: "Enter a price from 0 to 1,000,000 with at most two decimal places.",
    conflict: "Another edit changed the menu. Your form is still here: copy your changes, reload the menu, then apply them again.",
    maximum: "A service can have at most 20 duration options.", serviceId: "Service ID", assigned: "Assigned when published"
  },
  bg: {
    help: "Редактирайте услуга от менюто. Запазването я публикува веднага; посетителите виждат промяната след презареждане на сайта.",
    choose: "Услуга", add: "Добавете услуга", reload: "Презаредете менюто", title: "Име", description: "Описание",
    category: "Категория", available: "Предлага се за резервации", booking: "Връзка за резервации (https://… или tel:+…)",
    options: "Продължителност и цени", minutes: "Минути", price: "Цена", currency: "Валута", remove: "Премахнете варианта",
    addOption: "Добавете продължителност", priceHelp: "Оставете цената празна за уточняване при резервация. Използвайте точка или запетая и до два знака след нея. За услуга само с консултация премахнете всички варианти.",
    availabilityHelp: "Непредлаганите услуги остават видими, но без бутон за резервация. Съществуващите резервации не се променят.",
    save: "Запазете и публикувайте", publishing: "Публикуване…", published: "Услугата е публикувана. Презаредете сайта, за да видите промените.",
    dirty: "Непубликувани промени", clean: "Публикувано съдържание", newService: "Нова услуга",
    discard: "Да се отхвърлят ли непубликуваните промени по тази услуга?", priceError: "Въведете цена от 0 до 1 000 000 с най-много два знака след десетичния разделител.",
    conflict: "Менюто е променено от друго място. Формулярът Ви е запазен: копирайте промените си, презаредете менюто и ги приложете отново.",
    maximum: "Една услуга може да има най-много 20 варианта за продължителност.", serviceId: "Код на услугата", assigned: "Определя се при публикуване"
  }
},
  email: {
  en: { website: "Back to website", signIn: "Sign in", register: "Create an account", reset: "Reset your password", confirm: "Confirm email request", unsubscribe: "Unsubscribe from newsletters",
    email: "Email", name: "Name", phone: "Phone (optional)", password: "Password (15–128 characters)", repeat: "Repeat password", send: "Send verification link", finish: "Confirm", consent: "I want to receive newsletters and promotional offers by email. I can unsubscribe at any time.",
    requested: "If your request is eligible, an email will arrive shortly. Please allow a minute before requesting another link.", done: "Completed. You can sign in or return to your profile.", removed: "Your newsletter subscription has been removed.", mismatch: "The passwords do not match.", error: "The request could not be completed. Reopen the email link to check before retrying.", disabled: "This feature is disabled.", help: "First verify that you own the email address. You will choose a password after opening the link.", confirmHelp: "Press Confirm to apply the request from your email. Simply opening this page changes nothing." },
  bg: { website: "Към сайта", signIn: "Вход", register: "Създаване на профил", reset: "Възстановяване на парола", confirm: "Потвърждение на заявка", unsubscribe: "Отписване от бюлетина",
    email: "Имейл", name: "Име", phone: "Телефон (незадължителен)", password: "Парола (15–128 знака)", repeat: "Повторете паролата", send: "Изпратете връзка за потвърждение", finish: "Потвърдете", consent: "Желая да получавам бюлетини и промоционални предложения по имейл. Мога да се отпиша по всяко време.",
    requested: "Ако заявката е допустима, скоро ще получите имейл. Изчакайте поне минута, преди да поискате нова връзка.", done: "Готово. Можете да влезете или да се върнете в профила си.", removed: "Отписахте се от бюлетина.", mismatch: "Паролите не съвпадат.", error: "Заявката не може да бъде завършена. Отворете отново връзката от имейла, за да проверите, преди да опитате пак.", disabled: "Тази функция е изключена.", help: "Първо потвърдете, че имейл адресът е Ваш. Ще изберете парола след отваряне на връзката.", confirmHelp: "Натиснете „Потвърдете“, за да приложите заявката от имейла. Самото отваряне на страницата не променя нищо." }
},
  mail: {
    en: {
      title: "Email delivery", help: "Accepted means the provider has the message. Delivered means the recipient’s mail server accepted it; it does not confirm that it was read or placed in the inbox.",
      filter: "Status", all: "All messages", refresh: "Refresh latest", older: "Older messages", empty: "No messages match this filter.",
      local_outbox: "Local outbox — no external sending", resend: "Resend", healthy: "Worker is active", stale: "Worker has not checked in recently; check the backend", checked: "Last check", paused: "Provider requests paused until",
      queued: "Queued", processing: "Sending", accepted: "Accepted by provider", delivered: "Delivered to mail server", outbox: "Written to local outbox", delayed: "Delivery delayed", bounced: "Bounced", complained: "Spam complaint", skipped: "Skipped", failed: "Failed",
      challenge: "Account verification", notice: "Notification", newsletter: "Newsletter", reminder: "Appointment reminder", reference: "Reference", attempts: "Attempts", nextAttempt: "Next attempt / recovery after", unknownError: "Delivery needs attention",
      delivery_worker_error: "The delivery worker encountered a database error", reminder_scheduler_failed: "Reminders could not be scheduled; check database access", ineligible: "The message is no longer eligible", expired: "The message expired", transport_changed: "Transport changed after an earlier attempt; message was not resent", recipient_suppressed: "Sending is blocked after a bounce or complaint", retry_window_expired: "The safe retry window ended; check the provider before sending again", attempts_exhausted: "Maximum attempts reached", appointment_changed: "The appointment changed or was cancelled",
      provider_timeout: "The provider did not respond in time; acceptance is uncertain", provider_connection_failed: "Could not connect to the provider", provider_invalid_response: "Unexpected provider response; acceptance is uncertain", provider_configuration: "Check the API key, domain verification and provider access", provider_rate_limited: "The provider requested a slower sending rate", provider_unavailable: "The provider is temporarily unavailable", provider_rejected: "The provider rejected this message; check its dashboard", provider_quota_exceeded: "The provider’s sending allowance is exhausted", outbox_unavailable: "The private outbox could not be written",
      "email.bounced": "The recipient’s mail server permanently rejected the message", "email.complained": "The recipient reported this message as spam", "email.suppressed": "The provider blocked this recipient", "email.failed": "The provider reported a sending failure", "email.delivery_delayed": "The provider is still trying to deliver the message"
    },
    bg: {
      title: "Изпращане на имейли", help: "„Приет от доставчика“ означава, че доставчикът има съобщението. „Доставен до пощенския сървър“ не потвърждава прочитане или попадане във входящата папка.",
      filter: "Състояние", all: "Всички съобщения", refresh: "Обновете последните", older: "По-стари съобщения", empty: "Няма съобщения с този филтър.",
      local_outbox: "Локална изходяща папка — без реално изпращане", resend: "Resend", healthy: "Изпращането работи", stale: "Няма скорошна проверка от процеса за изпращане; проверете сървъра", checked: "Последна проверка", paused: "Заявките към доставчика са спрени до",
      queued: "На опашка", processing: "Изпраща се", accepted: "Приет от доставчика", delivered: "Доставен до пощенския сървър", outbox: "Записан в локалната папка", delayed: "Забавена доставка", bounced: "Върнат", complained: "Оплакване за спам", skipped: "Пропуснат", failed: "Неуспешен",
      challenge: "Потвърждение на профил", notice: "Известие", newsletter: "Бюлетин", reminder: "Напомняне за резервация", reference: "Номер", attempts: "Опити", nextAttempt: "Следващ опит / възстановяване след", unknownError: "Доставката изисква внимание",
      delivery_worker_error: "Процесът за изпращане срещна грешка в базата данни", reminder_scheduler_failed: "Напомнянията не могат да се планират; проверете достъпа до базата", ineligible: "Съобщението вече не е допустимо", expired: "Съобщението е изтекло", transport_changed: "Начинът за изпращане е сменен след предишен опит; съобщението не е изпратено повторно", recipient_suppressed: "Изпращането е блокирано след върнат имейл или оплакване", retry_window_expired: "Периодът за безопасен повторен опит е изтекъл; първо проверете доставчика", attempts_exhausted: "Достигнат е максималният брой опити", appointment_changed: "Резервацията е променена или отказана",
      provider_timeout: "Доставчикът не отговори навреме; приемането е неизвестно", provider_connection_failed: "Няма връзка с доставчика", provider_invalid_response: "Неочакван отговор от доставчика; приемането е неизвестно", provider_configuration: "Проверете API ключа, потвърждението на домейна и достъпа", provider_rate_limited: "Доставчикът изисква по-бавно изпращане", provider_unavailable: "Доставчикът временно не е достъпен", provider_rejected: "Доставчикът отхвърли съобщението; проверете неговия панел", provider_quota_exceeded: "Лимитът за изпращане на доставчика е изчерпан", outbox_unavailable: "Локалната изходяща папка не може да бъде записана",
      "email.bounced": "Пощенският сървър на получателя окончателно отхвърли съобщението", "email.complained": "Получателят маркира съобщението като спам", "email.suppressed": "Доставчикът блокира този получател", "email.failed": "Доставчикът съобщи за неуспешно изпращане", "email.delivery_delayed": "Доставчикът продължава да опитва доставка"
    }
  },
  newsletters: {
    en: { title: "Newsletters and offers", help: "Send only to verified subscribers in the selected language. Each message includes an unsubscribe link.", local: "Local demo: delivery writes to the private outbox. No external email is sent.", subject: "Subject", body: "Message (plain text)", locale: "Audience language", preview: "Preview recipients and message", send: "Queue this campaign", confirm: "Queue this campaign for the displayed subscribers?", audience: "Eligible recipients", queued: "Campaign queued. Delivery continues in the background.", history: "Recent campaigns", refresh: "Refresh delivery status", delivered: "Written to outbox", failed: "Failed", skipped: "Skipped", total: "Recipients" },
    bg: { title: "Бюлетини и предложения", help: "Изпращайте само до потвърдени абонати на избрания език. Всяко съобщение включва връзка за отписване.", local: "Локален пример: съобщенията се записват в личната изходяща папка. Не се изпраща реален имейл.", subject: "Тема", body: "Съобщение (обикновен текст)", locale: "Език на получателите", preview: "Преглед на получателите и съобщението", send: "Добавете кампанията за изпращане", confirm: "Да се добави ли кампанията за изпращане до показаните абонати?", audience: "Допустими получатели", queued: "Кампанията е добавена. Обработката продължава във фонов режим.", history: "Последни кампании", refresh: "Обновете състоянието", delivered: "Записани в изходящата папка", failed: "Неуспешни", skipped: "Пропуснати", total: "Получатели" }
  }
};

export function messages(section, language) {
  const locale = interfaceLocales.includes(language) ? language : "en";
  return dictionaries[section][locale];
}

export function accountIdentity(user, language) {
  if (!user) return "";
  const text = messages("accounts", language);
  const name = user.displayName && user.displayName !== user.email ? `${user.displayName} · ` : "";
  return `${text.signedInAs}: ${name}${user.email} · ${text[user.role] ?? user.role}`;
}

export function resolveLanguage(ui = {}, requested = new URLSearchParams(location.search).get("lang")) {
  if (interfaceLocales.includes(requested)) return requested;
  return interfaceLocales.includes(ui.defaultLocale) ? ui.defaultLocale : "en";
}

export function translate(root, language) {
  const text = messages("accounts", language);
  root.querySelectorAll("[data-text]").forEach(node => { node.textContent = text[node.dataset.text]; });
}

export function fillLocales(select, locales, selected, language) {
  const text = messages("accounts", language);
  select.replaceChildren(...locales.map(locale => new Option(text[locale] || locale, locale)));
  select.value = locales.includes(selected) ? selected : locales[0];
}

export function supportText(ui = {}, language) {
  return ui.supportText?.[language] || ui.supportText?.[ui.defaultLocale] || messages("accounts", language).contactAdmin;
}

export function applyPresentation(ui = {}, language, title) {
  document.documentElement.lang = language;
  document.title = ui.siteName ? `${title} · ${ui.siteName}` : title;
  document.querySelectorAll("[data-site-name]").forEach(node => {
    node.textContent = ui.siteName || ""; node.hidden = !ui.siteName;
  });
  document.querySelectorAll("[data-support]").forEach(node => { node.textContent = supportText(ui, language); });
}
