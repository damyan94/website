"""Real HTTP transport/webhook checks against a loopback-only simulated provider.

Called inside accounts_integration's disposable database. No provider credentials,
DNS, external HTTP requests or actual mailbox delivery are used.
"""
import base64
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import time
import uuid


def check_email_delivery(Client, password, sql, config_file, restart, command, env):
    original = json.loads(config_file.read_text())
    config = json.loads(json.dumps(original))
    key = b'local-webhook-test-secret-32bytes!'
    names = ('WEBSITE_TEST_MAIL_KEY', 'WEBSITE_TEST_WEBHOOK_KEY')
    previous = {name: env.get(name) for name in names}
    env[names[0]] = 're_local_test_only'
    env[names[1]] = 'whsec_' + base64.b64encode(key).decode()
    received, accepted, modes = defaultdict(list), {}, {}
    lock = threading.Lock()
    public, admin = Client(), Client()
    admin.login('admin@example.test', password)

    def wait_for(check, label, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = check()
            if result:
                return result
            time.sleep(.05)
        raise AssertionError('Timed out: ' + label)

    def webhook(provider_id, kind='email.delivered', event_id=None, timestamp=None,
                tamper=False, expected=200, raw=None, signature=None):
        body = raw or json.dumps({'type': kind, 'created_at': datetime.now(timezone.utc).isoformat(),
                                'data': {'email_id': provider_id}})
        event_id = event_id or 'msg_' + uuid.uuid4().hex
        stamp = str(timestamp if timestamp is not None else int(time.time()))
        digest = base64.b64encode(hmac.new(key, f'{event_id}.{stamp}.{body}'.encode(), hashlib.sha256).digest()).decode()
        if signature == 'rotation':
            signature = 'v1,not-the-current-key v1,' + digest
        headers = {'svix-id': event_id, 'svix-timestamp': stamp,
                   'svix-signature': signature or 'v1,' + digest,
                   'Origin': '', 'X-Accounts-Request': '', 'X-CSRF-Token': '', 'Sec-Fetch-Site': 'cross-site'}
        public.request('/api/v1/email/webhook/resend', 'POST', raw=body + (' ' if tamper else ''), headers=headers, expected=expected)
        return event_id

    class Provider(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            pass

        def do_POST(self):
            raw = self.rfile.read(int(self.headers['Content-Length']))
            payload = json.loads(raw)
            recipient = payload['to'][0]
            delivery_key = self.headers['Idempotency-Key']
            assert self.path == '/emails'
            assert self.headers['Authorization'] == 'Bearer re_local_test_only'
            with lock:
                received[recipient].append((delivery_key, raw))
                count = len(received[recipient])
                mode = modes.get(recipient, '')
                existing = accepted.get(delivery_key)
            status, response, after = 200, None, None
            if existing and existing[0] != raw:
                status, response = 409, {'name': 'invalid_idempotent_request'}
            elif mode == 'reject':
                status, response = 422, {'name': 'validation_error', 'message': 'PRIVATE PROVIDER DETAIL'}
            elif mode == 'auth':
                status, response = 403, {'name': 'restricted_api_key'}
            elif mode == 'limited' and count == 1:
                status, response, after = 429, {'name': 'rate_limit_exceeded'}, '120'
            elif mode == 'retry' and count == 1:
                status, response = 503, {'name': 'service_unavailable'}
            elif mode == 'badjson':
                response = 'PRIVATE MALFORMED RESPONSE'
            else:
                provider_id = existing[1] if existing else str(uuid.uuid4())
                with lock:
                    accepted[delivery_key] = (raw, provider_id)
                response = {'id': provider_id}
                if mode == 'early' and count == 1:
                    webhook(provider_id)
                if mode == 'timeout' and count == 1:
                    time.sleep(1.4)  # Provider accepted the message; caller loses the acknowledgment.
            encoded = json.dumps(response).encode() if isinstance(response, dict) else response.encode()
            try:
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(encoded)))
                if after:
                    self.send_header('Retry-After', after)
                self.end_headers()
                self.wfile.write(encoded)
            except (BrokenPipeError, ConnectionResetError):
                pass

    provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    worker = threading.Thread(target=provider.serve_forever, daemon=True)
    worker.start()
    settings = config['custom_config']['accounts']['customer_email']
    settings.update(transport='resend', resend={'api_key_env': names[0], 'webhook_secret_env': names[1],
                    'request_seconds': 1, 'test_port': provider.server_port})

    def queue(address, mode='', available='now()', kind='notice'):
        recipient = address + '@example.test'
        modes[recipient] = mode
        job = sql("INSERT INTO accounts.mail_jobs(kind,recipient,subject,body,available_at,expires_at) "
                  f"VALUES('{kind}','{recipient}','Test 🌺 <literal>','Private test body',{available},now()+interval '1 day') RETURNING id").splitlines()[0]
        return job, recipient

    def state(job):
        return sql('SELECT state FROM accounts.mail_jobs WHERE id=' + job)

    def due(job):
        sql('UPDATE accounts.mail_jobs SET available_at=now() WHERE id=' + job)
        sql('UPDATE accounts.mail_worker_status SET pause_until=now()')

    def provider_id(job):
        return sql('SELECT provider_id FROM accounts.mail_jobs WHERE id=' + job)

    try:
        config_file.write_text(json.dumps(config))
        restart()
        options = public.request('/api/v1/auth/options')[0]
        assert options['transport'] == 'resend' and 'resend' not in options
        assert env[names[0]] not in json.dumps(options)
        public.request('/api/v1/admin/mail', expected=401)
        dashboard = admin.request('/api/v1/admin/mail')[0]
        assert dashboard['transport'] == 'resend'
        for query in ['?state=unknown', '?before=1%20OR%201=1', '?token=secret']:
            admin.request('/api/v1/admin/mail' + query, expected=400)
        public.request('/api/v1/email/webhook/resend', 'POST', {}, expected=400)
        public.request('/api/v1/email/webhook/resend', expected=405)

        # Use a real registration request to check the existing business flow's provider payload.
        address = 'transport-registration@example.test'
        public.request('/api/v1/auth/register', 'POST', {'email': address, 'locale': 'bg'}, expected=202)
        job = sql("SELECT id FROM accounts.mail_jobs WHERE recipient='" + address + "' ORDER BY id DESC LIMIT 1")
        wait_for(lambda: state(job) == 'accepted', 'registration acceptance')
        payload = json.loads(received[address][0][1])
        assert payload['to'] == [address] and '#action=register&token=' in payload['text']
        assert payload['subject'] == 'Създаване на профил'
        assert sql('SELECT body FROM accounts.mail_jobs WHERE id=' + job) == ''
        assert sql('SELECT request_body IS NULL FROM accounts.mail_jobs WHERE id=' + job) == 't'
        identity = provider_id(job)
        webhook(identity, tamper=True, expected=400)
        webhook(identity, timestamp=int(time.time()) - 3600, expected=400)
        webhook(identity, timestamp=int(time.time()) + 3600, expected=400)
        webhook(identity, signature='v1,bogus', expected=400)
        assert state(job) == 'accepted'
        event = webhook(identity, signature='rotation')
        webhook(identity, event_id=event)
        webhook(identity, 'email.sent')
        webhook(identity, 'email.delivery_delayed')
        assert state(job) == 'delivered'
        assert sql("SELECT count(*) FROM accounts.mail_events WHERE event_id='" + event + "'") == '1'
        job, recipient = queue('transport-early', 'early')
        wait_for(lambda: state(job) == 'delivered', 'event before provider acknowledgment')
        print('PASS: real registration transport, private credentials, verified webhooks, replay and out-of-order delivery events', flush=True)

        # A timeout after provider acceptance must retry the exact same bytes and key after a restart.
        job, recipient = queue('transport-timeout', 'timeout')
        wait_for(lambda: state(job) == 'queued' and sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1', 'lost acknowledgment')
        failure = sql('SELECT last_error FROM accounts.mail_jobs WHERE id=' + job)
        assert failure == 'provider_timeout', failure
        settings['sender'] = 'changed-sender@example.test'
        env[names[1]] = env[names[1]].rstrip('=')  # Accept unpadded Base64 secrets too.
        config_file.write_text(json.dumps(config))
        restart()
        due(job)
        wait_for(lambda: state(job) == 'accepted', 'retry across restart')
        assert len(received[recipient]) == 2 and received[recipient][0] == received[recipient][1]
        assert sum(raw == received[recipient][0][1] for raw, _ in accepted.values()) == 1

        job, recipient = queue('transport-limited', 'limited')
        wait_for(lambda: state(job) == 'queued' and sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1', 'provider rate limit')
        assert sql('SELECT available_at>now()+interval \'100 seconds\' FROM accounts.mail_jobs WHERE id=' + job) == 't'
        assert admin.request('/api/v1/admin/mail')[0]['worker']['paused']
        time.sleep(.4)
        assert len(received[recipient]) == 1
        due(job)
        wait_for(lambda: state(job) == 'accepted', 'rate limited retry')

        job, recipient = queue('transport-retry', 'retry')
        wait_for(lambda: state(job) == 'queued' and sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1', '503 retry')
        due(job)
        wait_for(lambda: state(job) == 'accepted', '503 recovery')
        job, recipient = queue('transport-rejected', 'reject')
        wait_for(lambda: state(job) == 'failed', 'permanent failure')
        assert sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1'
        assert sql('SELECT body=\'\' AND request_body IS NULL FROM accounts.mail_jobs WHERE id=' + job) == 't'
        assert 'PRIVATE PROVIDER DETAIL' not in json.dumps(admin.request('/api/v1/admin/mail')[0])

        job, recipient = queue('transport-badjson', 'badjson')
        wait_for(lambda: state(job) == 'queued' and sql('SELECT attempts FROM accounts.mail_jobs WHERE id=' + job) == '1', 'malformed provider response')
        assert sql('SELECT last_error FROM accounts.mail_jobs WHERE id=' + job) == 'provider_invalid_response'
        sql('UPDATE accounts.mail_jobs SET attempts=4 WHERE id=' + job)
        due(job)
        wait_for(lambda: state(job) == 'failed', 'exhausted retries')
        assert sql('SELECT request_body IS NULL FROM accounts.mail_jobs WHERE id=' + job) == 't'

        job, recipient = queue('transport-auth', 'auth')
        wait_for(lambda: state(job) == 'failed', 'invalid provider credentials')
        assert sql('SELECT last_error FROM accounts.mail_jobs WHERE id=' + job) == 'provider_configuration'
        assert admin.request('/api/v1/admin/mail')[0]['worker']['paused']
        sql('UPDATE accounts.mail_worker_status SET pause_until=now()')

        job, recipient = queue('transport-retry-expired', available="now()+interval '1 day'")
        sql("UPDATE accounts.mail_jobs SET transport='resend',first_attempt_at=now()-interval '24 hours',attempts=1,available_at=now() WHERE id=" + job)
        wait_for(lambda: state(job) == 'failed', 'expired provider idempotency window')
        assert sql('SELECT last_error FROM accounts.mail_jobs WHERE id=' + job) == 'retry_window_expired'
        assert not received[recipient]
        print('PASS: timeout recovery with identical requests, provider backoff, permanent errors and bounded idempotency window', flush=True)

        job, recipient = queue('transport-bounce')
        wait_for(lambda: state(job) == 'accepted', 'bounce fixture')
        webhook(provider_id(job), 'email.bounced')
        webhook(provider_id(job), 'email.delivered')
        assert state(job) == 'bounced'
        retry_job, _ = queue('transport-bounce')
        wait_for(lambda: state(retry_job) == 'skipped', 'suppression after bounce')
        assert len(received[recipient]) == 1
        webhook(provider_id(job), 'email.complained')
        webhook(provider_id(job), 'email.sent')
        assert state(job) == 'complained'
        job, _ = queue('transport-failed-event')
        wait_for(lambda: state(job) == 'accepted', 'failure event fixture')
        webhook(provider_id(job), 'email.failed')
        assert state(job) == 'failed'

        # An unauthenticated caller must not gain the operator/admin delivery view.
        account = 'transport-operator@example.test'
        admin.request('/api/v1/admin/users', 'POST', {'email': account, 'displayName': 'Mail operator', 'phone': '', 'locale': 'en', 'role': 'operator', 'password': password}, expected=201)
        operator = Client(); operator.login(account, password)
        operator.request('/api/v1/admin/mail', expected=403)
        customer_id = admin.request('/api/v1/admin/users', 'POST', {'email': 'transport-customer@example.test', 'displayName': 'Mail customer', 'phone': '', 'locale': 'en', 'role': 'customer', 'password': password}, expected=201)[0]['id']
        customer = Client(); customer.login('transport-customer@example.test', password)
        customer.request('/api/v1/admin/mail', expected=403)
        assert customer_id
        dashboard = admin.request('/api/v1/admin/mail')[0]
        assert not any(key in json.dumps(dashboard) for key in ['delivery_key', 'request_body', 'webhookKey', 'Private test body'])
        assert any(item['error'] == 'recipient_suppressed' for item in dashboard['jobs'])
        sql("INSERT INTO accounts.mail_jobs(kind,recipient,subject,body,state) SELECT 'notice','page@example.test','Page test','','failed' FROM generate_series(1,55)")
        page = admin.request('/api/v1/admin/mail?state=failed')[0]
        assert len(page['jobs']) == 50 and page['nextBefore']
        following = admin.request('/api/v1/admin/mail?state=failed&before=' + page['nextBefore'])[0]
        assert not set(j['id'] for j in page['jobs']) & set(j['id'] for j in following['jobs'])
        print('PASS: bounce/complaint suppression, safe failure visibility and administrator-only delivery access', flush=True)

        # Configuration errors must fail before any external request can happen.
        for change in [{'transport': 'smtp'}, {'resend': {**settings['resend'], 'request_seconds': 0}},
                       {'resend': {**settings['resend'], 'api_key_env': 'MISSING_TEST_MAIL_KEY'}}]:
            invalid = json.loads(json.dumps(config))
            invalid['custom_config']['accounts']['customer_email'].update(change)
            config_file.write_text(json.dumps(invalid))
            command(config_file, '--migrate-accounts', success=False)
        invalid = json.loads(json.dumps(config))
        invalid['custom_config']['accounts']['public_origin'] = 'https://website.example.test'
        config_file.write_text(json.dumps(invalid))
        command(config_file, '--migrate-accounts', success=False)  # test_port cannot be used with a production origin.
    finally:
        config_file.write_text(json.dumps(original))
        sql('UPDATE accounts.mail_worker_status SET pause_until=now()')
        restart()
        provider.shutdown()
        provider.server_close()
        worker.join(timeout=5)
        for name, value in previous.items():
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
