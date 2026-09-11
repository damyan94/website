"""Reservation checks used by the isolated accounts integration fixture."""
from concurrent.futures import ThreadPoolExecutor
from datetime import date, datetime, timedelta
import json
import subprocess
import time
from pathlib import Path
import uuid
from urllib.parse import urlencode
from zoneinfo import ZoneInfo


def check_reservations(Client, admin, customer, operator, anonymous, sql, config_file, command, restart):
    anonymous.request('/api/v1/reservations/options', expected=401)
    anonymous.request('/api/v1/reservations', 'POST', {}, expected=401)
    customer.request('/api/v1/admin/reservations?date=2026-09-10&days=7', expected=403)
    options = customer.request('/api/v1/reservations/options')[0]
    assert options['timezone'] == 'Europe/Sofia'
    assert {s['id'] for s in options['services']} == {'relax', 'deep-tissue'}
    assert sql('SELECT version FROM reservations.schema_version') == '2'
    assert sql("SELECT count(*) FROM pg_extension WHERE extname='btree_gist'") == '1'
    day = str(date.fromisoformat(options['today']) + timedelta(days=3))
    next_day = str(date.fromisoformat(day) + timedelta(days=1))

    def slots(client=customer, chosen_day=day, **extra):
        return client.request('/api/v1/reservations/availability?' + urlencode({
            'serviceId': 'relax', 'durationMinutes': '30', 'revision': options['revision'], 'date': chosen_day, **extra}))[0]['slots']

    available = slots()
    assert available
    for slot in available:
        local = datetime.fromisoformat(slot['startsAt'].replace('Z', '+00:00')).astimezone(ZoneInfo(options['timezone']))
        assert local.date().isoformat() == day and 11 <= local.hour < 20 and local.minute % 15 == 0
        assert (local + timedelta(minutes=45)).hour <= 20
    first = available[0]['startsAt']
    def payload(start=first, chosen_day=day, **extra):
        return {'serviceId': 'relax', 'durationMinutes': 30, 'revision': options['revision'],
            'date': chosen_day, 'startsAt': start, 'therapistId': '', 'requestKey': str(uuid.uuid4()), **extra}

    body = payload()
    for staff in (admin, operator):
        error = staff.request('/api/v1/reservations', 'POST', payload(), expected=403)[0]
        assert 'Only customers can book for themselves' in error['error']
    customer.request('/api/v1/reservations', 'POST', body, expected=403)  # Email proof is required.
    sql("UPDATE accounts.users SET email_verified_at=now() WHERE email='customer2@example.test'")
    customer.request('/api/v1/reservations', 'POST', body, expected=403, headers={'X-CSRF-Token': 'invalid'})
    customer.request('/api/v1/reservations', 'POST', body, expected=403, headers={'Origin': 'https://evil.example'})
    customer.request('/api/v1/reservations', 'POST', {**body, 'priceMinor': 1}, expected=400)
    customer.request('/api/v1/reservations', 'POST', {**body, 'guest': {}}, expected=403)
    created = customer.request('/api/v1/reservations', 'POST', body, expected=201)[0]['appointment']
    assert created['priceMinor'] == 4000 and created['userId'] == customer.request('/api/v1/me')[0]['user']['id']
    assert created['contactEmail'] == 'customer2@example.test'
    jobs = sql('SELECT count(*) FROM accounts.mail_jobs')
    assert customer.request('/api/v1/reservations', 'POST', body)[0]['appointment']['id'] == created['id']
    assert sql('SELECT count(*) FROM accounts.mail_jobs') == jobs
    # A customer promoted to staff cannot replay a self-booking request key.
    sql("UPDATE accounts.users SET role='operator' WHERE email='customer2@example.test'")
    customer.request('/api/v1/reservations', 'POST', body, expected=403)
    assert customer.request('/api/v1/me/reservations')[0]['total'] == 1  # Preserve existing history.
    sql("UPDATE accounts.users SET role='customer' WHERE email='customer2@example.test'")
    customer.request('/api/v1/reservations', 'POST', {**body, 'durationMinutes': 60}, expected=409)
    customer.request('/api/v1/reservations', 'POST', payload(), expected=409)
    customer.request('/api/v1/reservations', 'POST', payload(available[1]['startsAt']), expected=409)
    assert first not in {s['startsAt'] for s in slots()}
    assert available[3]['startsAt'] in {s['startsAt'] for s in slots()}  # Half-open buffer boundary.
    assert operator.request('/api/v1/admin/reservations?' + urlencode({'date':day,'days':'7'}))[0]['total'] == 1
    assert admin.request('/api/v1/me/reservations')[0]['total'] == 0
    guest = {'name':'Test guest','email':'guest@example.test','phone':'','locale':'en'}
    guest_body = payload(slots(chosen_day=next_day)[0]['startsAt'], next_day, guest=guest)
    for phone in ['abc', '+359/888', '1'*33]:
        operator.request('/api/v1/reservations', 'POST', {**guest_body,'guest':{**guest,'phone':phone}},expected=400)
    for invalid_id in ['', '1 OR 1=1', '1'*19, '１２']:
        customer.request('/api/v1/reservations', 'PATCH', {'id':invalid_id,'version':'1','action':'cancelled'},expected=400)
    made_guest = operator.request('/api/v1/reservations', 'POST', guest_body, expected=201)[0]['appointment']
    assert made_guest['userId'] is None
    assert sql("SELECT count(*) FROM accounts.users WHERE email='guest@example.test'") == '0'
    customer.request('/api/v1/reservations', 'PATCH', {'id':made_guest['id'],'version':made_guest['version'],'action':'cancelled'},expected=404)
    for query in ['date=not-a-date','date=2026-02-30','therapistId=missing','durationMinutes=-1','revision=wrong']:
        fields = dict(serviceId='relax',durationMinutes='30',revision=options['revision'],date=day)
        key,value=query.split('=',1); fields[key]=value
        customer.request('/api/v1/reservations/availability?'+urlencode(fields),expected=400)
    customer.request('/api/v1/reservations/availability?'+urlencode(dict(serviceId='relax',durationMinutes='30',revision='f'*64,date=day)),expected=409)
    customer.request('/api/v1/reservations/availability?'+urlencode(dict(serviceId='relax',durationMinutes='30',revision=options['revision'],date=day,excludeId=created['id'])),expected=403)
    print('PASS: customer-only self-booking, staff guests, role changes, shared validation, prices, buffers and idempotency', flush=True)

    move_slot = slots(operator, next_day)[0]['startsAt']
    move = {'id':created['id'],'version':created['version'],'action':'reschedule','revision':options['revision'],'date':next_day,'startsAt':move_slot,'therapistId':''}
    customer.request('/api/v1/reservations','PATCH',move,expected=403)
    conflict = {**move, 'startsAt':made_guest['startsAt']}
    operator.request('/api/v1/reservations','PATCH',conflict,expected=409)
    assert sql('SELECT count(*) FROM reservations.occupancy WHERE appointment_id='+created['id']) == '2'
    moved = operator.request('/api/v1/reservations','PATCH',move)[0]['appointment']
    assert moved['priceMinor'] == created['priceMinor'] and moved['version'] != created['version']
    operator.request('/api/v1/reservations','PATCH',move,expected=409)
    assert first in {s['startsAt'] for s in slots()}
    # Two independent clients race for the same resources; only one may commit.
    race_start = slots()[0]['startsAt']
    def race(client):
        try:
            return client.request('/api/v1/reservations','POST',payload(race_start, guest=guest),expected=201)[0]['appointment']
        except AssertionError as error:
            assert error.args[0][2] == 409, error
            return None
    with ThreadPoolExecutor(max_workers=2) as pool:
        raced = list(pool.map(race,[admin,operator]))
    assert sum(a is not None for a in raced)==1
    # The exclusion constraint protects direct SQL writes too.
    try:
        sql('UPDATE reservations.occupancy SET during=(SELECT during FROM reservations.occupancy WHERE appointment_id='+moved['id']+" AND resource_id='demo-room') WHERE appointment_id="+made_guest['id']+" AND resource_id='demo-room'")
        raise AssertionError('Expected exclusion constraint failure')
    except subprocess.CalledProcessError:
        pass
    operator.request('/api/v1/reservations','PATCH',{'id':moved['id'],'version':moved['version'],'action':'completed'},expected=409)
    customer.request('/api/v1/reservations','PATCH',{'id':moved['id'],'version':moved['version'],'action':'cancelled'})
    assert customer.request('/api/v1/me/reservations?scope=history')[0]['total']==1
    assert customer.request('/api/v1/me/reservations')[0]['total']==0
    assert sql('SELECT count(*) FROM reservations.occupancy WHERE appointment_id='+moved['id'])=='0'
    print('PASS: operator rescheduling, stale versions, cancellation/history, atomic conflicts and database exclusion', flush=True)

    # Existing snapshots survive subsequent menu edits and process restarts.
    config = json.loads(config_file.read_text())
    content_file = Path(config['custom_config']['public_content_file'])
    content = json.loads(content_file.read_text())
    next(s for s in content['services'] if s['id']=='relax')['variants'][0]['priceMinor']=4100
    content_file.write_text(json.dumps(content,ensure_ascii=False))
    restart()
    fresh = customer.request('/api/v1/reservations/options')[0]
    assert fresh['revision'] != options['revision']
    assert customer.request('/api/v1/me/reservations?scope=history')[0]['appointments'][0]['priceMinor']==4000
    # A closure can block the whole studio without changing user accounts.
    config['custom_config']['reservations']['closures']=[{'date':day,'resources':[]}]
    config_file.write_text(json.dumps(config)); restart(); options=fresh
    assert slots()==[]
    config['custom_config']['reservations']['closures']=[]
    config_file.write_text(json.dumps(config)); restart()
    assert slots()
    command(config_file,'--migrate-reservations')
    assert sql('SELECT count(*) FROM reservations.events') != '0'
    # Elapsed time alone never completes an appointment or grants visit credit.
    sql("UPDATE reservations.appointments SET starts_at=now()-interval '2 hours',ends_at=now()-interval '1 hour' WHERE id="+made_guest['id'])
    sql("UPDATE reservations.occupancy SET during=tstzrange(now()-interval '2 hours',now()-interval '45 minutes','[)') WHERE appointment_id="+made_guest['id'])
    assert sql('SELECT state FROM reservations.appointments WHERE id='+made_guest['id'])=='confirmed'
    completed=operator.request('/api/v1/reservations','PATCH',{'id':made_guest['id'],'version':made_guest['version'],'action':'completed'})[0]['appointment']
    assert completed['state']=='completed'
    operator.request('/api/v1/reservations','PATCH',{'id':completed['id'],'version':completed['version'],'action':'cancelled'},expected=409)
    late_slot=slots(chosen_day=next_day)[0]['startsAt']
    late=customer.request('/api/v1/reservations','POST',payload(late_slot,next_day),expected=201)[0]['appointment']
    sql("UPDATE reservations.appointments SET starts_at=now()+interval '1 hour',ends_at=now()+interval '90 minutes' WHERE id="+late['id'])
    customer.request('/api/v1/reservations','PATCH',{'id':late['id'],'version':late['version'],'action':'cancelled'},expected=403)
    operator.request('/api/v1/reservations','PATCH',{'id':late['id'],'version':late['version'],'action':'cancelled'})
    # Daylight-saving boundaries use UTC instants while honoring every local minute.
    zone=ZoneInfo(options['timezone']); today=date.fromisoformat(options['today']); transitions=[]
    for offset in range(1,365):
        d=today+timedelta(days=offset)
        begin=datetime.combine(d,datetime.min.time(),zone)
        end=datetime.combine(d+timedelta(days=1),datetime.min.time(),zone)
        minutes=int((end.timestamp()-begin.timestamp())/60)
        if minutes!=1440: transitions.append((d,minutes))
    assert len(transitions)==2
    dst_config=json.loads(json.dumps(config)); rules=dst_config['custom_config']['reservations']
    rules.update(horizon_days=365,lead_minutes=0,business_hours=[{'days':[1,2,3,4,5,6,7],'start':'01:00','end':'05:00'}])
    for resource in rules['resources']: resource['hours']=rules['business_hours']
    for service in rules['services']: service['buffer_after']=0
    config_file.write_text(json.dumps(dst_config)); restart()
    for d,minutes in transitions:
        times=slots(chosen_day=str(d))
        assert len(times)==(240+minutes-1440-30)//15+1
        assert len({s['startsAt'] for s in times})==len(times)
        local_three=[s for s in times if datetime.fromisoformat(s['startsAt'].replace('Z','+00:00')).astimezone(zone).strftime('%H:%M')=='03:00']
        assert len(local_three)==(2 if minutes>1440 else 0)
    config_file.write_text(json.dumps(config)); restart()
    print('PASS: explicit completion, cancellation deadlines and both daylight-saving transitions', flush=True)
    print('PASS: immutable prices, reservation persistence, configuration closures and repeatable migration', flush=True)

    if config['custom_config']['accounts']['customer_email']['enabled']:
        outbox = Path(config['custom_config']['accounts']['customer_email']['outbox_directory'])

        def wait_for(check, description):
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                result = check()
                if result:
                    return result
                time.sleep(.05)
            raise AssertionError('Timed out: ' + description)

        def book_reminder(label, aged=True):
            contact = {**guest, 'email': label + '@example.test', 'locale': 'bg'}
            appointment = operator.request('/api/v1/reservations', 'POST',
                payload(slots(operator, next_day)[0]['startsAt'], next_day, guest=contact), expected=201)[0]['appointment']
            # Simulate an appointment entering the reminder window after time has passed.
            sql("UPDATE reservations.appointments SET starts_at=now()+interval '23 hours',ends_at=now()+interval '23 hours 30 minutes'" +
                (",updated_at=now()-interval '2 days'" if aged else '') + ' WHERE id=' + appointment['id'])
            return appointment

        def parked_reminder(appointment):
            job = sql("INSERT INTO accounts.mail_jobs(kind,recipient,subject,body,available_at,expires_at) "
                      f"VALUES('reminder','{appointment['contactEmail']}','Reminder','Fixture',now()+interval '1 day',now()+interval '23 hours') RETURNING id").splitlines()[0]
            sql(f"INSERT INTO reservations.reminders VALUES({appointment['id']},{appointment['version']},{job})")
            return job

        due = book_reminder('reminder-due')
        restart()
        reminder = wait_for(lambda: sql('SELECT mail_job_id FROM reservations.reminders WHERE appointment_id=' + due['id']), 'durable reminder queue')
        wait_for(lambda: sql('SELECT state FROM accounts.mail_jobs WHERE id=' + reminder) == 'delivered', 'reminder outbox')
        message = json.loads((outbox / (reminder + '.json')).read_text())
        assert message['kind'] == 'reminder' and message['to'] == 'reminder-due@example.test'
        assert 'Напомняне' in message['subject'] and 'Europe/Sofia' in message['text']
        local_time = sql("SELECT to_char(starts_at AT TIME ZONE 'Europe/Sofia','YYYY-MM-DD HH24:MI') FROM reservations.appointments WHERE id=" + due['id'])
        assert local_time in message['text']
        restart()
        assert sql('SELECT count(*) FROM reservations.reminders WHERE appointment_id=' + due['id']) == '1'

        cancelled = book_reminder('reminder-cancelled')
        cancelled_job = parked_reminder(cancelled)
        operator.request('/api/v1/reservations', 'PATCH', {'id': cancelled['id'], 'version': cancelled['version'], 'action': 'cancelled'})
        assert sql('SELECT state FROM accounts.mail_jobs WHERE id=' + cancelled_job) == 'skipped'
        assert not (outbox / (cancelled_job + '.json')).exists()

        changed = book_reminder('reminder-moved')
        changed_job = parked_reminder(changed)
        moved = operator.request('/api/v1/reservations', 'PATCH', {'id': changed['id'], 'version': changed['version'],
            'action': 'reschedule', 'revision': options['revision'], 'date': next_day,
            'startsAt': slots(operator, next_day)[0]['startsAt'], 'therapistId': ''})[0]['appointment']
        assert sql('SELECT state FROM accounts.mail_jobs WHERE id=' + changed_job) == 'skipped'
        sql("UPDATE reservations.appointments SET starts_at=now()+interval '23 hours',ends_at=now()+interval '23 hours 30 minutes',updated_at=now()-interval '2 days' WHERE id=" + moved['id'])
        restart()
        second = wait_for(lambda: sql('SELECT mail_job_id FROM reservations.reminders WHERE appointment_id=' + moved['id'] + ' AND appointment_version=' + moved['version']), 'reminder after rescheduling')
        wait_for(lambda: sql('SELECT state FROM accounts.mail_jobs WHERE id=' + second) == 'delivered', 'rescheduled reminder delivery')
        assert second != changed_job and not (outbox / (changed_job + '.json')).exists()

        disabled = book_reminder('reminder-disabled')
        disabled_job = parked_reminder(disabled)
        flags = json.loads(json.dumps(config))
        flags['custom_config']['reservations']['reminders_enabled'] = False
        config_file.write_text(json.dumps(flags)); restart()
        sql('UPDATE accounts.mail_jobs SET available_at=now() WHERE id=' + disabled_job)
        wait_for(lambda: sql('SELECT state FROM accounts.mail_jobs WHERE id=' + disabled_job) == 'skipped', 'disabled reminder delivery')
        config_file.write_text(json.dumps(config)); restart()

        last_minute = book_reminder('reminder-short-notice', aged=False)
        past = book_reminder('reminder-past')
        sql("UPDATE reservations.appointments SET starts_at=now()-interval '1 hour',ends_at=now()-interval '30 minutes' WHERE id=" + past['id'])
        restart()
        # A due sentinel proves the scheduler has completed a scan, without a fixed long sleep.
        sentinel = book_reminder('reminder-sentinel'); restart()
        wait_for(lambda: sql('SELECT mail_job_id FROM reservations.reminders WHERE appointment_id=' + sentinel['id']), 'scheduler sentinel')
        assert sql('SELECT count(*) FROM reservations.reminders WHERE appointment_id IN (' + last_minute['id'] + ',' + past['id'] + ')') == '0'
        print('PASS: localized 24-hour reminders, restart deduplication, cancellation/rescheduling, disabled reminders and short-notice/expired visits', flush=True)
