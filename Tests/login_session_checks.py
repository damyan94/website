"""Characterize login's verification/transaction boundary using the real database."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import select
import subprocess
import time


def check_login_sessions(Client, password, sql, psql, env, absolute_seconds):
    email = 'login-devices@example.test'
    race_email = 'login-recheck@example.test'
    ids = []
    for address in [email, race_email]:
        sql("INSERT INTO accounts.users(email,password_hash,display_name,locale,role) "
            "SELECT '" + address + "',password_hash,'Login test','en','customer' "
            "FROM accounts.users WHERE email='admin@example.test'")
        ids.append(sql("SELECT id FROM accounts.users WHERE email='" + address + "'"))
    user, race_user = ids
    subjects = ','.join(ids)

    def state():
        return (
            sql("SELECT id,password_hash,enabled,role,version,credential_version,last_login_at FROM accounts.users WHERE id IN (" + subjects + ") ORDER BY id"),
            sql("SELECT * FROM accounts.sessions ORDER BY token_hash"),
            sql("SELECT * FROM accounts.audit WHERE subject_id IN (" + subjects + ") ORDER BY id"),
        )

    def session_hash(client):
        return hashlib.sha256(client.cookie.split('=', 1)[1].encode()).hexdigest()

    def expired_sessions():
        sql("INSERT INTO accounts.sessions(token_hash,user_id,csrf_token,last_seen,expires_at) VALUES "
            "(repeat('e',64)," + race_user + ",repeat('a',64),now(),now()-interval '1 second'),"
            "(repeat('f',64)," + race_user + ",repeat('b',64),now()-interval '2 hours',now()+interval '1 hour')")

    # Successful login cleans expired/idle sessions globally, including another account.
    expired_sessions()
    devices = [Client() for _ in range(6)]
    tokens = []
    for client in devices:
        reply, headers = client.login(email, password)
        assert set(reply) == {'user', 'csrfToken'}
        assert reply['user'] == {'id':user, 'email':email, 'displayName':'Login test', 'phone':'',
                                 'locale':'en', 'role':'customer', 'enabled':True, 'version':'1'}
        token = client.cookie.split('=', 1)[1]
        assert len(token) == 64 and len(reply['csrfToken']) == 64
        assert token != reply['csrfToken']
        token_hash = session_hash(client)
        assert sql("SELECT csrf_token FROM accounts.sessions WHERE token_hash='" + token_hash + "'") == reply['csrfToken']
        assert sql("SELECT created_at=last_seen AND expires_at-created_at=make_interval(secs=>" + str(absolute_seconds) + ") FROM accounts.sessions WHERE token_hash='" + token_hash + "'") == 't'
        assert 'Max-Age=' + str(absolute_seconds) in next(v for k, v in headers.items() if k.lower() == 'set-cookie')
        tokens.append(token_hash)
        assert set(sql("SELECT token_hash FROM accounts.sessions WHERE user_id=" + user).splitlines()) == set(tokens[-5:])
    assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + race_user) == '0'
    devices[0].request('/api/v1/me', expected=401)
    # Replacing a current device happens before eviction, so the other four survive.
    old_token = tokens[-1]
    devices[-1].login(email, password)
    tokens[-1] = session_hash(devices[-1])
    assert tokens[-1] != old_token
    assert set(sql("SELECT token_hash FROM accounts.sessions WHERE user_id=" + user).splitlines()) == set(tokens[-5:])
    assert sql("SELECT version,credential_version FROM accounts.users WHERE id=" + user) == '1|1'
    assert sql("SELECT count(*) FROM accounts.audit WHERE actor_id=" + user + " AND subject_id=" + user + " AND action='login'") == '7'
    before = state()
    rejected = Client(); rejected.cookie = devices[-1].cookie
    error, _ = rejected.login(email, 'Wrong password long enough', expected=401)
    assert error == {'error':'Email or password is incorrect'} and rejected.cookie == ''
    assert state() == before  # Failed login clears the browser cookie, but does not revoke the stored session.
    print('PASS: login response, stored session/hash/expiry, global cleanup, five-device limit and cookie replacement', flush=True)

    def blocked_login(client, address, expected, lock, change=None):
        # An interactive psql connection holds a real lock. Observe the HTTP worker
        # waiting on it before changing credentials or cancelling its audit statement.
        blocker = subprocess.Popen(
            [str(psql), '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', env['ALOHA_DATABASE_URL']],
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            blocker.stdin.write("BEGIN; SET LOCAL statement_timeout='5s'; " + lock + ";\n")
            blocker.stdin.flush()
            assert select.select([blocker.stdout], [], [], 5)[0], 'Lock holder did not become ready'
            holder = int(blocker.stdout.readline())
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(client.login, address, password, expected)
                try:
                    deadline = time.monotonic() + 2
                    worker = ''
                    while time.monotonic() < deadline:
                        worker = sql("SELECT pid FROM pg_stat_activity WHERE " + str(holder) +
                                     "=ANY(pg_blocking_pids(pid)) AND query LIKE '" +
                                     ("SELECT pg_advisory_xact_lock%" if change else "INSERT INTO accounts.audit%") + "'")
                        if worker:
                            break
                        assert not pending.done(), 'Login did not reach the expected lock'
                        time.sleep(.01)
                    assert worker, 'Login did not block before the deadline'
                    if change:
                        blocker.stdin.write(change + ";\n")
                        blocker.stdin.flush()
                    else:
                        assert sql("SELECT pg_cancel_backend(" + worker + ")") == 't'
                        pending.result(timeout=8)  # Observe rollback while the audit table remains locked.
                finally:
                    _, errors = blocker.communicate("COMMIT;\n", timeout=5)
                assert blocker.returncode == 0, errors
                return pending.result(timeout=8)
        finally:
            if blocker.poll() is None:
                blocker.kill()
                blocker.communicate(timeout=5)

    advisory_lock = "SELECT pg_backend_pid() FROM pg_advisory_xact_lock(70123002)"
    race = Client()
    for change in ["password_hash=NULL,credential_version=credential_version+1",
                   "enabled=false,credential_version=credential_version+1"]:
        before = state()
        error, _ = blocked_login(race, race_email, 401, advisory_lock,
                                 "UPDATE accounts.users SET " + change + " WHERE id=" + race_user)
        assert error == {'error':'Email or password is incorrect'} and race.cookie == ''
        assert state()[1:] == before[1:]
        assert sql("SELECT last_login_at IS NULL FROM accounts.users WHERE id=" + race_user) == 't'
        sql("UPDATE accounts.users SET password_hash=(SELECT password_hash FROM accounts.users WHERE email='admin@example.test'),enabled=true WHERE id=" + race_user)
    # The existing recheck tests hash/enabled, and returns current profile/version;
    # a credential-version increment alone does not reject an otherwise valid login.
    reply, _ = blocked_login(race, race_email, 200, advisory_lock,
                             "UPDATE accounts.users SET role='operator',display_name='Updated during login',"
                             "version=version+1,credential_version=credential_version+1 WHERE id=" + race_user)
    assert reply['user']['role'] == 'operator' and reply['user']['displayName'] == 'Updated during login'
    assert reply['user']['version'] == sql("SELECT version FROM accounts.users WHERE id=" + race_user)
    assert sql("SELECT count(*) FROM accounts.audit WHERE subject_id=" + race_user + " AND action='login'") == '1'
    print('PASS: login rechecks changed credentials/enabled state under the lock and returns current account data', flush=True)

    # Cancel at audit insertion, after cleanup, old-cookie deletion, eviction and
    # session insertion have run. Every change must roll back, including other users' sessions.
    expired_sessions()
    before = state()
    interrupted = Client(); interrupted.cookie = race.cookie
    cookie = interrupted.cookie
    error, _ = blocked_login(interrupted, email, 503,
                             "LOCK TABLE accounts.audit IN SHARE MODE; SELECT pg_backend_pid()")
    assert error == {'error':'Accounts are temporarily unavailable'} and interrupted.cookie == cookie
    assert state() == before
    # A successful cross-account login removes the old cookie's session too.
    previous_hash = session_hash(race)
    interrupted.login(email, password)
    assert session_hash(interrupted) != previous_hash
    assert sql("SELECT count(*) FROM accounts.sessions WHERE token_hash='" + previous_hash + "'") == '0'
    assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + user) == '5'
    assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + race_user) == '0'
    print('PASS: interrupted login rolls back cleanup/eviction/issuance/audit; cross-account login replaces the old session', flush=True)
