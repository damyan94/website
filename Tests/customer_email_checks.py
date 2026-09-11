"""Customer email checks, called inside accounts_integration's private fixture.

All addresses use .test and delivery is filesystem-only. Database edits simulate
expiry/time passing and interrupted delivery, without weakening runtime settings.
"""
import hashlib
import json
from pathlib import Path
import re
import secrets
import stat
import time


def check_customer_email(Client, admin_password, sql, config_file, restart, command):
    config = json.loads(config_file.read_text())
    outbox = Path(config['custom_config']['accounts']['customer_email']['outbox_directory'])
    public = Client()
    admin = Client()
    admin.login('admin@example.test', admin_password)
    assert sql('SELECT version FROM accounts.schema_version') == '4'
    assert not admin.request('/api/v1/me')[0]['user']['emailVerified']
    assert sql('SELECT count(*) FROM accounts.login_identities') == '0'
    options = public.request('/api/v1/auth/options')[0]
    assert {key: value for key, value in options.items() if key != 'ui'} == {'email': True, 'registration': True, 'newsletters': True, 'transport': 'local_outbox', 'locales': ['bg', 'en']}
    assert options['ui']['siteName'] == config['custom_config']['accounts']['ui']['site_name']
    for path in ['/email', '/accounts-assets/email.js', '/accounts-assets/newsletters.js']:
        payload, headers = public.request(path)
        assert payload and {k.lower(): v for k, v in headers.items()}['referrer-policy'] == 'no-referrer'
    public.request('/api/v1/admin/newsletters', expected=401)
    for provider in ['google', 'facebook']:
        public.request('/api/v1/auth/' + provider, expected=404)

    def wait_for(check, description):
        deadline = time.monotonic() + 65
        while time.monotonic() < deadline:
            result = check()
            if result:
                return result
            time.sleep(.1)
        raise AssertionError('Timed out: ' + description)

    def mail(recipient, action=None, campaign=None):
        def find():
            for file in sorted(outbox.glob('*.json'), key=lambda p: int(p.stem), reverse=True):
                message = json.loads(file.read_text())
                if message['to'] != recipient:
                    continue
                if action:
                    if f'#action={action}&token=' not in message['text']:
                        continue
                    latest = sql("SELECT token_hash FROM accounts.email_challenges WHERE email='" + recipient + "' AND purpose='" + action + "' ORDER BY id DESC LIMIT 1")
                    if hashlib.sha256(token(message).encode()).hexdigest() != latest:
                        continue
                if campaign and sql('SELECT campaign_id FROM accounts.mail_jobs WHERE id=' + message['id']) != campaign:
                    continue
                if sql('SELECT state FROM accounts.mail_jobs WHERE id=' + message['id']) != 'delivered':
                    continue
                if action:
                    assert message['text'].startswith(options['ui']['siteName'] + '\n')
                assert stat.S_IMODE(file.stat().st_mode) == 0o600
                assert stat.S_IMODE(outbox.stat().st_mode) == 0o700
                return message
        return wait_for(find, 'private outbox delivery')

    def token(message):
        return re.search(r'&token=([a-f0-9]{64})', message['text'])[1]

    def elapsed(email):
        # Addresses are fixed test constants, never arbitrary input.
        sql("UPDATE accounts.email_challenges SET created_at=created_at-interval '2 minutes' WHERE email='" + email + "'")

    def request_link(email, action='register'):
        route = 'register' if action == 'register' else 'forgot-password'
        public.request('/api/v1/auth/' + route, 'POST', {'email': email, 'locale': 'en'}, expected=202)
        return token(mail(email, action))

    def register(email, subscribed=False, locale='en'):
        public.request('/api/v1/auth/register', 'POST', {'email': email, 'locale': locale}, expected=202)
        proof = token(mail(email, 'register'))
        password = secrets.token_urlsafe(24)
        body = {'token': proof, 'displayName': 'Customer 🌺', 'phone': '+359 123', 'password': password, 'newsletter': subscribed}
        public.request('/api/v1/auth/complete-registration', 'POST', body, expected=201)
        client = Client(); client.login(email, password)
        return client, password

    email = 'newsletter-customer@example.test'
    proof = request_link(email)
    for after_restart in [False, True]:
        if after_restart:
            restart()
        public.request('/api/v1/auth/register', 'POST', {'email': email, 'locale': 'en'}, expected=202)
        assert sql("SELECT count(*) FROM accounts.email_challenges WHERE email='" + email + "'") == '1'
    assert sql("SELECT count(*) FROM accounts.users WHERE email='" + email + "'") == '0'
    public.request('/email?lang=en')  # Opening a link does not consume it.
    digest = hashlib.sha256(proof.encode()).hexdigest()
    assert sql("SELECT consumed_at IS NULL FROM accounts.email_challenges WHERE token_hash='" + digest + "'") == 't'
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': proof}, expected=400)
    password = secrets.token_urlsafe(24)
    body = {'token': proof, 'displayName': 'Customer 🌺', 'phone': '', 'password': password, 'newsletter': False}
    public.request('/api/v1/auth/complete-registration', 'POST', {**body, 'role': 'admin'}, expected=400)
    public.request('/api/v1/auth/complete-registration', 'POST', {**body, 'phone': '123x'}, expected=400)
    assert sql("SELECT consumed_at IS NULL FROM accounts.email_challenges WHERE token_hash='" + digest + "'") == 't'
    public.request('/api/v1/auth/complete-registration', 'POST', body, expected=201)
    public.request('/api/v1/auth/complete-registration', 'POST', body, expected=400)
    public.request('/api/v1/me', expected=401)
    customer = Client(); customer.login(email, password)
    own = customer.request('/api/v1/me')[0]['user']
    assert own['role'] == 'customer' and own['emailVerified'] and not own['newsletterSubscribed']
    customer.request('/api/v1/admin/newsletters', expected=403)
    customer.request('/api/v1/admin/newsletters/preview', 'POST', {}, expected=403)
    initial_id = own['id']
    print('PASS: registration after ownership proof, single-use/purpose-bound tokens, default-off consent and role boundaries', flush=True)

    # Neither an existing registration nor an unknown recovery leaks eligibility.
    known = public.request('/api/v1/auth/register', 'POST', {'email': email, 'locale': 'en'}, expected=202)[0]
    unknown = public.request('/api/v1/auth/forgot-password', 'POST', {'email': 'absent@example.test', 'locale': 'en'}, expected=202)[0]
    legacy = public.request('/api/v1/auth/forgot-password', 'POST', {'email': 'admin@example.test', 'locale': 'en'}, expected=202)[0]
    assert known == unknown == legacy
    assert sql("SELECT count(*) FROM accounts.email_challenges WHERE email IN ('absent@example.test','admin@example.test')") == '0'
    admin.request('/api/v1/me/newsletter', 'POST', {'subscribed': True}, expected=409)
    admin.request('/api/v1/me/verify-email', 'POST', {}, expected=202)
    verify = token(mail('admin@example.test', 'verify'))
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': verify})
    assert admin.request('/api/v1/me')[0]['user']['emailVerified']
    elapsed(email)
    reset = request_link(email, 'reset')
    sql("UPDATE accounts.email_challenges SET expires_at=now()-interval '1 second' WHERE token_hash='" + hashlib.sha256(reset.encode()).hexdigest() + "'")
    public.request('/api/v1/auth/reset-password', 'POST', {'token': reset, 'password': password}, expected=400)
    elapsed(email)
    reset = request_link(email, 'reset')
    other_session = Client(); other_session.login(email, password)
    replacement = secrets.token_urlsafe(24)
    public.request('/api/v1/auth/reset-password', 'POST', {'token': reset, 'password': replacement})
    public.request('/api/v1/auth/reset-password', 'POST', {'token': reset, 'password': password}, expected=400)
    customer.request('/api/v1/me', expected=401)
    other_session.request('/api/v1/me', expected=401)
    customer.login(email, replacement)
    # Another pending reset cannot survive a subsequent authenticated password change.
    elapsed(email)
    stale = request_link(email, 'reset')
    customer.request('/api/v1/me/password', 'POST', {'currentPassword': replacement, 'newPassword': password})
    public.request('/api/v1/auth/reset-password', 'POST', {'token': stale, 'password': replacement}, expected=400)
    customer.login(email, password)
    print('PASS: legacy verification, generic recovery, expiry, session revocation and credential-version invalidation', flush=True)

    restart()  # Keep the subsequent independent campaign tests below HTTP throttles.
    elapsed(email)
    customer.request('/api/v1/me/newsletter', 'POST', {'subscribed': True}, expected=202)
    assert not customer.request('/api/v1/me')[0]['user']['newsletterSubscribed']
    subscribe = token(mail(email, 'subscribe'))
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': subscribe})
    assert customer.request('/api/v1/me')[0]['user']['newsletterSubscribed']
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': subscribe}, expected=400)
    bg, bg_password = register('bulgarian-subscriber@example.test', True, 'bg')
    register('non-subscriber@example.test')

    def preview(language='en', expected=1):
        payload = {'requestKey': secrets.token_hex(16), 'locale': language, 'subject': 'September offer', 'text': 'A plain text offer.\nNo tracking pixels.'}
        draft = admin.request('/api/v1/admin/newsletters/preview', 'POST', payload)[0]
        assert draft['recipients'] == expected and '[Unsubscribe' in draft['text']
        assert admin.request('/api/v1/admin/newsletters/preview', 'POST', payload)[0]['id'] == draft['id']
        admin.request('/api/v1/admin/newsletters/preview', 'POST', {**payload, 'subject': 'Changed'}, expected=409)
        return draft

    def send(draft, expected=202):
        return admin.request('/api/v1/admin/newsletters/send', 'POST', {'id': draft['id'], 'expectedRecipients': draft['recipients']}, expected=expected)

    draft = preview()
    send(draft)
    send(draft, expected=200)
    assert sql('SELECT count(*) FROM accounts.mail_jobs WHERE campaign_id=' + draft['id']) == '1'
    message = mail(email, campaign=draft['id'])
    assert message['headers']['List-Unsubscribe-Post'] == 'List-Unsubscribe=One-Click'
    unsub = token(message)
    public.request('/email')
    assert customer.request('/api/v1/me')[0]['user']['newsletterSubscribed']
    assert sql('SELECT body FROM accounts.mail_jobs WHERE id=' + message['id']) == ''
    link = '/api/v1/newsletter/one-click?token=' + unsub
    # A mail scanner's GET or malformed POST must not unsubscribe.
    try:
        public.request(link, expected=405)
    except AssertionError as error:
        assert error.args[0][2] == 404
    public.request(link, 'POST', raw='anything', headers={'Content-Type': 'application/x-www-form-urlencoded'}, expected=400)
    assert customer.request('/api/v1/me')[0]['user']['newsletterSubscribed']
    public.request(link, 'POST', raw='List-Unsubscribe=One-Click', headers={'Origin': '', 'X-Accounts-Request': '', 'X-CSRF-Token': '', 'Sec-Fetch-Site': 'cross-site', 'Content-Type': 'application/x-www-form-urlencoded'})
    public.request('/api/v1/newsletter/unsubscribe', 'POST', {'token': unsub})
    assert not customer.request('/api/v1/me')[0]['user']['newsletterSubscribed']
    assert bg.request('/api/v1/me')[0]['user']['newsletterSubscribed']
    print('PASS: double opt-in, language-filtered recipients, idempotent campaigns, private outbox and one-click unsubscribe', flush=True)

    restart()
    changed = preview('bg')
    bg.request('/api/v1/me/newsletter', 'POST', {'subscribed': False})
    send(changed, expected=409)
    elapsed('bulgarian-subscriber@example.test')
    bg.request('/api/v1/me/newsletter', 'POST', {'subscribed': True}, expected=202)
    bg_consent = token(mail('bulgarian-subscriber@example.test', 'subscribe'))
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': bg_consent})
    retry_draft = preview('bg')
    parked = outbox.with_suffix('.parked')
    outbox.rename(parked)  # Deliberate delivery failure; no credentials leave the fixture.
    try:
        send(retry_draft)
        job = sql('SELECT id FROM accounts.mail_jobs WHERE campaign_id=' + retry_draft['id'])
        wait_for(lambda: sql('SELECT state FROM accounts.mail_jobs WHERE id=' + job) == 'queued' and sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1', 'failed delivery retry')
    finally:
        parked.rename(outbox)
    restart()
    sql("UPDATE accounts.mail_jobs SET available_at=now() WHERE id=" + job)
    mail('bulgarian-subscriber@example.test', campaign=retry_draft['id'])
    assert sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '2'
    assert len(list(outbox.glob(job + '.json'))) == 1

    cancelled = preview('bg')
    outbox.rename(parked)
    try:
        send(cancelled)
        job = sql('SELECT id FROM accounts.mail_jobs WHERE campaign_id=' + cancelled['id'])
        wait_for(lambda: sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1' and sql('SELECT state FROM accounts.mail_jobs WHERE id=' + job) == 'queued', 'queued campaign after transport failure')
        bg.request('/api/v1/me/newsletter', 'POST', {'subscribed': False})
        assert sql('SELECT state FROM accounts.mail_jobs WHERE id=' + job) == 'skipped'
    finally:
        parked.rename(outbox)
    assert not (outbox / (job + '.json')).exists()
    assert int(sql("SELECT count(*) FROM accounts.subscription_events WHERE action='unsubscribed'")) >= 3
    print('PASS: changed audience rejection, durable delivery retry across restart and cancellation of queued promotions', flush=True)

    restart()
    # Email changes require password + verified new mailbox, retain the user ID,
    # revoke sessions and never transfer consent automatically to another address.
    elapsed(email)
    # Reset the persistent fixture throttle window after exercising five link types.
    sql("UPDATE accounts.email_challenges SET created_at=now()-interval '2 hours' WHERE email='" + email + "'")
    customer.request('/api/v1/me/newsletter', 'POST', {'subscribed': True}, expected=202)
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': token(mail(email, 'subscribe'))})
    customer.request('/api/v1/me/email', 'POST', {'email': 'changed@example.test', 'currentPassword': 'Wrong password long enough'}, expected=403)
    customer.request('/api/v1/me/email', 'POST', {'email': 'changed@example.test', 'currentPassword': password}, expected=202)
    change = token(mail('changed@example.test', 'change'))
    assert customer.request('/api/v1/me')[0]['user']['email'] == email
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': change})
    customer.request('/api/v1/me', expected=401)
    customer.login('changed@example.test', password)
    moved = customer.request('/api/v1/me')[0]['user']
    assert moved['id'] == initial_id and moved['emailVerified'] and not moved['newsletterSubscribed']
    assert sql('SELECT bool_and(length(token_hash)=64) FROM accounts.email_challenges') == 't'
    assert sql('SELECT bool_and(length(token_hash)=64) FROM accounts.newsletter_links') == 't'
    public.request('/Runtime/Email/' + job + '.json', expected=404)
    public.request('/accounts-assets/../../Runtime/Email/' + job + '.json', expected=403)
    # The original migration remains unchanged; repeated CLI migration is safe.
    command(config_file, '--migrate-accounts')
    assert customer.request('/api/v1/me')[0]['user']['id'] == initial_id
    unsafe = json.loads(json.dumps(config))
    unsafe['custom_config']['accounts']['customer_email']['outbox_directory'] = config['app']['document_root'] + '/mail'
    invalid = config_file.with_name('unsafe-email.json')
    invalid.write_text(json.dumps(unsafe))
    command(invalid, '--migrate-accounts', success=False)
    print('PASS: verified email changes, stable account IDs, removed old consent, hashed capabilities and private-outbox configuration', flush=True)

    # Disable only registration/newsletters: recovery remains available, and
    # previously queued promotions must not escape the new configuration.
    flags = json.loads(json.dumps(config))
    flags['custom_config']['accounts']['customer_email'].update(registration_enabled=False, newsletters_enabled=False)
    # Restore real consent so the configuration flag is the only reason to skip.
    elapsed('bulgarian-subscriber@example.test')
    bg.request('/api/v1/me/newsletter', 'POST', {'subscribed': True}, expected=202)
    public.request('/api/v1/auth/confirm-email', 'POST', {'token': token(mail('bulgarian-subscriber@example.test', 'subscribe'))})
    sql("UPDATE accounts.mail_jobs j SET state='queued',attempts=0,available_at=now()+interval '1 day',subscription_generation=(SELECT generation FROM accounts.subscriptions s WHERE s.user_id=j.user_id) WHERE j.id=" + job)
    try:
        config_file.write_text(json.dumps(flags))
        restart()
        options = public.request('/api/v1/auth/options')[0]
        assert options['email'] and not options['registration'] and not options['newsletters']
        public.request('/api/v1/auth/register', 'POST', {}, expected=404)
        public.request('/api/v1/admin/newsletters', expected=404)
        customer.request('/api/v1/me')
        public.request('/api/v1/auth/forgot-password', 'POST', {'email': 'absent@example.test', 'locale': 'en'}, expected=202)
        sql('UPDATE accounts.mail_jobs SET available_at=now() WHERE id=' + job)
        wait_for(lambda: sql('SELECT state FROM accounts.mail_jobs WHERE id=' + job) == 'skipped', 'disabled newsletter delivery')
        assert not (outbox / (job + '.json')).exists()
    finally:
        config_file.write_text(json.dumps(config))
        restart()
    print('PASS: independently disabled registration/newsletters preserve accounts and suppress queued delivery', flush=True)
