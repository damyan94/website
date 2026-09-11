"""Content checks used by accounts_integration.py --content-editor.

All writes target the caller's disposable fixture, never the real example.
"""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import json
from pathlib import Path
import re
import subprocess
import time


ENDPOINT = '/api/v1/admin/content/services'


def editable(service):
    return {key: deepcopy(service.get(key, True if key == 'available' else None))
            for key in ['id', 'title', 'description', 'category', 'variants', 'available', 'bookingUrl']}


def check_content_editor(admin, customer, operator, anonymous, config_path, command, binary, env, log):
    config = json.loads(config_path.read_text())
    source = Path(config['custom_config']['public_content_file'])
    backups = Path(config['custom_config']['content_editor']['backups_directory'])
    assert source.parent == config_path.parent  # protect the real content
    initial_bytes = source.read_bytes()
    original = json.loads(initial_bytes)
    anonymous.request(ENDPOINT, expected=401)
    for client in [anonymous, customer, operator]:
        for method in ['GET', 'PATCH', 'POST']:
            client.request(ENDPOINT, method, {} if method != 'GET' else None,
                           expected=401 if client is anonymous else 403)
    data = admin.request(ENDPOINT)[0]
    assert data['services'] == original['services'] and data['locales'] == ['bg', 'en']
    assert data['currencies'] == ['EUR'] and data['defaultBookingUrl'].startswith('tel:+359')
    script, headers = anonymous.request('/accounts-assets/content.js')
    assert b'createServiceEditor' in script
    assert any(k.lower() == 'content-security-policy' for k in headers)
    service = editable(data['services'][0])
    request = {'revision': data['revision'], 'service': service}
    admin.request(ENDPOINT, 'PATCH', request, expected=403, headers={'Origin': 'https://evil.example'})
    admin.request(ENDPOINT, 'PATCH', request, expected=403, headers={'X-CSRF-Token': 'bad'})
    admin.request(ENDPOINT, 'PATCH', request, expected=403, headers={'X-Accounts-Request': ''})
    admin.request(ENDPOINT, 'PATCH', expected=400, raw=b'{"revision":"a","revision":"b"}')
    admin.request(ENDPOINT, 'PATCH', expected=413, raw=b' ' * (256 * 1024 + 1))

    cases = [
        {'title': {'bg': 'Only one language'}},
        {'title': {**service['title'], 'fr': 'Unexpected'}},
        {'description': {**service['description'], 'en': ' '}},
        {'title': {**service['title'], 'en': 'x' * 201}},
        {'title': {**service['title'], 'en': 'bad\x00text'}},
        {'category': 'missing'}, {'available': 'true'}, {'id': 1},
        {'bookingUrl': 'javascript:alert(1)'}, {'bookingUrl': '//evil.example'},
        {'bookingUrl': 'https://good.example\\@evil.example'},
        {'bookingUrl': 'https://name:password@example.test'},
        {'bookingUrl': 'https://example.test:99999'}, {'bookingUrl': 'tel:bad'},
        {'variants': [{'durationMinutes': 60, 'priceMinor': -1, 'currency': 'EUR'}]},
        {'variants': [{'durationMinutes': 60, 'priceMinor': 1.5, 'currency': 'EUR'}]},
        {'variants': [{'durationMinutes': 60, 'priceMinor': 100000001, 'currency': 'EUR'}]},
        {'variants': [{'durationMinutes': 60, 'priceMinor': '10', 'currency': 'EUR'}]},
        {'variants': [{'durationMinutes': 0, 'priceMinor': 10, 'currency': 'EUR'}]},
        {'variants': [{'durationMinutes': 60.5, 'priceMinor': 10, 'currency': 'EUR'}]},
        {'variants': [{'durationMinutes': 60, 'priceMinor': 10, 'currency': 'USD'}]},
        {'variants': [{'durationMinutes': 60, 'priceMinor': 10, 'currency': 'EUR', 'extra': True}]},
        {'variants': [{'durationMinutes': 60, 'priceMinor': 10, 'currency': 'EUR'}] * 2},
        {'extra': True}
    ]
    for changes in cases:
        admin.request(ENDPOINT, 'PATCH', {**request, 'service': {**service, **changes}}, expected=400)
    admin.request(ENDPOINT, 'PATCH', {**request, 'service': {**service, 'id': 'missing'}}, expected=404)
    admin.request(ENDPOINT, 'POST', request, expected=400)  # client cannot choose IDs
    assert source.read_bytes() == initial_bytes and not list(backups.glob('*.json'))
    print('PASS: editor authorization, CSRF/origin, strict bodies, translations, URLs and integer-price validation', flush=True)

    service['title']['en'] = 'Edited test service'
    service['description']['bg'] = 'Първи ред\nВтори ред 🌺 <script>literal text</script>'
    service['available'] = False
    service['bookingUrl'] = 'tel:+359879925093'
    service['variants'] = [
        {'durationMinutes': 45, 'priceMinor': 1234, 'currency': 'EUR'},
        {'durationMinutes': 90, 'priceMinor': None, 'currency': 'EUR'}
    ]
    updated = admin.request(ENDPOINT, 'PATCH', {'revision': data['revision'], 'service': service})[0]
    assert updated['revision'] != data['revision'] and updated['serviceId'] == service['id']
    published = anonymous.request('/api/v1/site')[0]
    assert published == json.loads(source.read_bytes())
    assert editable(published['services'][0]) == service
    assert {k: v for k, v in published.items() if k != 'services'} == {k: v for k, v in original.items() if k != 'services'}
    assert published['services'][1:] == original['services'][1:]
    empty, headers = anonymous.request('/api/v1/site', 'HEAD')
    assert empty == b'' and int(next(v for k, v in headers.items() if k.lower() == 'content-length')) > 0
    backup = json.loads((backups / (updated['backupId'] + '.json')).read_bytes())
    assert backup['document'] == original and backup['previousRevision'] == data['revision']
    assert backup['replacementRevision'] == updated['revision'] and backup['actorId'] == admin.request('/api/v1/me')[0]['user']['id']
    assert backups.stat().st_mode & 0o077 == 0 and source.stat().st_mode & 0o077 == 0
    for path in ['/Content/site.json', '/Runtime/ContentBackups/' + updated['backupId'] + '.json']:
        anonymous.request(path, expected=404)
    count = len(list(backups.glob('*.json')))
    unchanged = admin.request(ENDPOINT, 'PATCH', {'revision': updated['revision'], 'service': service})[0]
    assert unchanged['revision'] == updated['revision'] and len(list(backups.glob('*.json'))) == count
    admin.request(ENDPOINT, 'PATCH', request, expected=409)

    # Multiple writers using one revision yield exactly one successful publish.
    def publish(index):
        from accounts_integration import Client
        client = Client()
        client.cookie, client.csrf = admin.cookie, admin.csrf
        change = deepcopy(service)
        change['title']['en'] = 'Concurrent edit ' + str(index)
        try:
            return client.request(ENDPOINT, 'PATCH', {'revision': updated['revision'], 'service': change})[0]
        except AssertionError as error:
            assert error.args[0][2] == 409
            return None

    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(publish, range(2)))
    assert sum(result is not None for result in results) == 1
    data = next(result for result in results if result is not None)
    service = editable(data['services'][0])
    new_service = deepcopy(service)
    new_service.update(id='', variants=[], available=True)
    created = admin.request(ENDPOINT, 'POST', {'revision': data['revision'], 'service': new_service}, expected=201)[0]
    assert re.fullmatch(r'service-[0-9a-f]{24}', created['serviceId'])
    assert len(created['services']) == len(original['services']) + 1
    assert created['services'][-1]['variants'] == []

    # Readers must observe a complete old or new document during a save.
    change = editable(created['services'][0])
    change['title']['en'] = 'Snapshot during concurrent reads'
    before = anonymous.request('/api/v1/site')[0]
    expected = deepcopy(before)
    expected['services'][0].update(change)
    from accounts_integration import Client
    def public_read(_):
        return Client().request('/api/v1/site')[0]
    with ThreadPoolExecutor(max_workers=6) as pool:
        readers = [pool.submit(public_read, i) for i in range(60)]
        created = admin.request(ENDPOINT, 'PATCH', {'revision': created['revision'], 'service': change})[0]
        assert all(reader.result() in (before, expected) for reader in readers)
    assert anonymous.request('/api/v1/site')[0] == expected
    print('PASS: live public snapshots, private backups, unchanged-field preservation, stale/concurrent edits and service creation', flush=True)

    # A completed external edit must be detected; unrelated fields survive saves.
    manual = json.loads(source.read_bytes())
    manual['services'][0]['futureField'] = {'keep': True}
    source.write_text(json.dumps(manual))
    admin.request(ENDPOINT, 'PATCH', {'revision': created['revision'], 'service': service}, expected=409)
    data = admin.request(ENDPOINT)[0]
    service['title']['en'] = 'Preserve future service field'
    data = admin.request(ENDPOINT, 'PATCH', {'revision': data['revision'], 'service': service})[0]
    assert data['services'][0]['futureField'] == {'keep': True}
    # Repeat the external-read/publish transition to catch intermittent stalls.
    for index in range(6):
        manual = json.loads(source.read_bytes())
        manual['services'][0]['futureField'] = {'keep': True, 'round': index}
        source.write_text(json.dumps(manual))
        data = admin.request(ENDPOINT)[0]
        service['title']['en'] = 'Repeated publication ' + str(index)
        started = time.monotonic()
        data = admin.request(ENDPOINT, 'PATCH', {'revision': data['revision'], 'service': service})[0]
        elapsed = time.monotonic() - started
        if elapsed > 1:
            print(f'Content save {index} took {elapsed:.2f}s', flush=True)
        assert data['services'][0]['futureField']['round'] == index
    saved_bytes = source.read_bytes()
    source.write_text('{broken')
    admin.request(ENDPOINT, 'GET', expected=503)
    admin.request(ENDPOINT, 'PATCH', {'revision': data['revision'], 'service': service}, expected=503)
    assert source.read_text() == '{broken'
    assert anonymous.request('/api/v1/site')[0] == json.loads(saved_bytes)
    source.write_bytes(saved_bytes)

    # A missing backup directory must prevent publication before source rename.
    unavailable = backups.with_name(backups.name + '-unavailable')
    backups.rename(unavailable)
    try:
        failed = deepcopy(service)
        failed['title']['en'] = 'Must never be published'
        admin.request(ENDPOINT, 'PATCH', {'revision': data['revision'], 'service': failed}, expected=503)
        assert source.read_bytes() == saved_bytes
    finally:
        unavailable.rename(backups)

    # A second server on a different port cannot publish to the same file.
    other_config = deepcopy(config)
    other_config['listeners'][0]['port'] = 18084
    other_config['custom_config']['accounts']['public_origin'] = 'http://127.0.0.1:18084'
    other_path = config_path.with_name('content-rejected.json')
    other_path.write_text(json.dumps(other_config))
    result = subprocess.run([str(binary), '--config=' + str(other_path)], env=env, stdout=log, stderr=log, timeout=10)
    assert result.returncode != 0
    # CLI commands do not try to own the content lock while the server runs.
    command(config_path, '--migrate-accounts')
    for bad_config in [
        {**other_config, 'custom_config': {**other_config['custom_config'], 'accounts': {'enabled': False}}},
        {**other_config, 'custom_config': {**other_config['custom_config'], 'content_editor': {
            **other_config['custom_config']['content_editor'], 'backups_directory': config['app']['document_root']}}}
    ]:
        other_path.write_text(json.dumps(bad_config))
        result = subprocess.run([str(binary), '--config=' + str(other_path)], env=env, stdout=log, stderr=log, timeout=10)
        assert result.returncode != 0
    print('PASS: external-edit conflicts, invalid source preservation, failed-backup handling, exclusive file ownership and configuration safeguards', flush=True)
