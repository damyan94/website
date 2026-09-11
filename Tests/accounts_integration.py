#!/usr/bin/env python3
"""Exercise real C++ routes against an isolated, disposable PostgreSQL cluster."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
from http.cookies import SimpleCookie
import json
import re
import os
from pathlib import Path
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / 'Examples/AlohaMassage'
LOCAL = ROOT / 'Build/LocalPostgres/root'


def binaries():
    pg = LOCAL / 'usr/lib/postgresql/15/bin'
    if not (pg / 'initdb').exists():
        pg_config = shutil.which('pg_config')
        if not pg_config:
            raise RuntimeError('Install PostgreSQL or prepare Build/LocalPostgres first')
        pg = Path(subprocess.check_output([pg_config, '--bindir'], text=True).strip())
    env = os.environ.copy()
    local_lib = LOCAL / 'usr/lib/x86_64-linux-gnu'
    if local_lib.exists():
        env['LD_LIBRARY_PATH'] = str(local_lib) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    return pg, env


class Client:
    def __init__(self, port=18082, origin=None):
        self.port = port
        self.origin = origin or f'http://127.0.0.1:{port}'
        self.cookie = ''
        self.csrf = ''

    def request(self, path, method='GET', body=None, expected=200, headers=None, raw=None):
        h = {'Host': self.origin.split('://', 1)[1]}
        if self.cookie:
            h['Cookie'] = self.cookie
        if method not in ('GET', 'HEAD'):
            h.update({'Origin': self.origin, 'Content-Type': 'application/json', 'X-Accounts-Request': '1', 'X-CSRF-Token': self.csrf})
        if headers:
            h.update(headers)
        # Content requests may wait for filesystem durability; unrelated routes
        # retain their shorter timeout to catch account/HTTP regressions.
        timeout = 65 if path.startswith('/api/v1/admin/content/') else 12
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)
        connection.request(method, path, body=raw if raw is not None else json.dumps(body).encode() if body is not None else None, headers=h)
        response = connection.getresponse()
        payload = response.read()
        response_headers = dict(response.getheaders())
        content_type = response.getheader('Content-Type', '')
        data = json.loads(payload) if payload and 'application/json' in content_type else payload
        assert response.status == expected, (path, method, response.status, expected, data)
        cookie = response.getheader('Set-Cookie')
        if cookie:
            parsed = SimpleCookie(cookie)
            self.cookie = '; '.join(f'{key}={m.value}' for key, m in parsed.items() if m.value)
        if isinstance(data, dict) and 'csrfToken' in data:
            self.csrf = data['csrfToken']
        connection.close()
        return data, response_headers

    def login(self, email, password, expected=200):
        return self.request('/api/v1/auth/login', 'POST', {'email': email, 'password': password}, expected)


def check_admin_modules():
    """Exercise actual module lifecycle and translation functions without a browser."""
    subprocess.run(['node', '--input-type=module', '-'], cwd=ROOT, check=True, input=r'''
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const load = async name => import('data:text/javascript;base64,' + Buffer.from(await readFile('Admin/' + name)).toString('base64'));
const i18n = await load('i18n.js');
assert.equal(i18n.resolveLanguage({defaultLocale:'bg'}, null), 'bg');
assert.equal(i18n.resolveLanguage({defaultLocale:'bg'}, 'en'), 'en');
assert.equal(i18n.resolveLanguage({defaultLocale:'bg'}, 'constructor'), 'bg');
for (const section of ['accounts','services','email','newsletters','reservations','mail']) {
  assert.deepEqual(Object.keys(i18n.messages(section,'en')).sort(), Object.keys(i18n.messages(section,'bg')).sort());
}
const html = await readFile('Admin/index.html','utf8');
for (const [,key] of html.matchAll(/data-text="([^"]+)"/g)) {
  assert.equal(typeof i18n.messages('accounts','en')[key], 'string', key);
}
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(match => match[1]);
assert.equal(new Set(ids).size, ids.length);
class Node {
  children=[]; parent=null; events={}; attributes={};
  constructor(tag) {this.tag=tag;}
  append(...nodes) { for(const node of nodes) {node.remove(); this.children.push(node); node.parent=this;} }
  remove() {if(this.parent) this.parent.children=this.parent.children.filter(child=>child!==this); this.parent=null;}
  replaceChildren(...nodes) {for(const child of [...this.children]) child.remove(); this.append(...nodes);}
  addEventListener(name,callback) {this.events[name]=callback;}
  setAttribute(name,value) {this.attributes[name]=value;}
}
globalThis.document={createElement:tag=>new Node(tag)};
globalThis.Option=class extends Node {constructor(label,value){super('option');this.textContent=label;this.value=value;}};
const select=new Node('select'); i18n.fillLocales(select,['fr'],'bg','bg');
assert.equal(select.value,'fr'); // Site languages need not include either UI language.
const brand=new Node('strong'), support=new Node('p');
document.documentElement={}; document.querySelectorAll=selector=>selector==='[data-site-name]'?[brand]:[support];
i18n.applyPresentation({siteName:'<script>literal</script>',defaultLocale:'en',supportText:{en:'Shop support'}},'bg','Accounts');
assert.equal(brand.textContent,'<script>literal</script>'); assert.equal(support.textContent,'Shop support');
const {accountAccess,accountModules,createModuleHost}=await load('modules.js');
assert.deepEqual(accountAccess(null,{reservations:true}),{canBook:false,canManage:false});
assert.equal(i18n.accountIdentity(null,'en'),'');
assert.equal(i18n.accountIdentity({displayName:'Test User',email:'person@example.test',role:'customer'},'en'),
  'Signed in as: Test User · person@example.test · Customer');
assert.equal(i18n.accountIdentity({displayName:'',email:'staff@example.test',role:'operator'},'bg'),
  `Влезли сте като: staff@example.test · ${i18n.messages('accounts','bg').operator}`);
const browserEvents=new Map();
globalThis.window={location:{hash:'#section-optional'},history:{
  replaceState(_state,_title,hash){window.location.hash=hash;},
  pushState(_state,_title,hash){window.location.hash=hash;}
},addEventListener(name,callback){browserEvents.set(name,callback);},
removeEventListener(name,callback){if(browserEvents.get(name)===callback) browserEvents.delete(name);},scrollTo(){}};
let profile={user:{id:'1',role:'admin'}};
const context={isAdminPage:true,getProfile:()=>profile,features:{contentEditor:true},options:{newsletters:true},text:i18n.messages('accounts','en'),run:fn=>fn()};
const visible=()=>accountModules.filter(m=>m.enabled(context)).map(m=>m.id);
assert.deepEqual(visible(),['services','newsletters','users','profile']);
for(const role of ['customer','operator']) {profile.user.role=role;assert.deepEqual(visible(),[]);}
profile.user.role='admin';context.features.contentEditor=false;context.options.newsletters=false;
assert.deepEqual(visible(),['users','profile']);context.isAdminPage=false;assert.deepEqual(visible(),['profile']);
context.features.reservations=true;assert.deepEqual(visible(),['profile']);
for(const role of ['admin','operator','customer']) {
  profile.user.role=role;
  assert.deepEqual(accountAccess(profile.user,context.features),{canBook:role==='customer',canManage:role!=='customer'});
  assert.deepEqual(visible(),role==='customer'?['reservations','profile']:['profile']);
}
context.isAdminPage=true;profile.user.role='customer';assert.deepEqual(visible(),[]);
context.isAdminPage=true;profile.user.role='operator';assert.deepEqual(visible(),['reservations','profile']);
context.features.reservations=false;
const container=new Node('main'), navigation=new Node('nav');
let loaded=0, disposed=0, updated=0, fail=true, enabled=true;
const definitions=[{id:'stable',title:'profile',enabled:()=>true,create:async()=>({load(){loaded++;},update(){updated++;},dispose(){disposed++;}})},
  {id:'optional',title:'users',enabled:()=>enabled,create:async()=>({load(){if(fail) throw new Error('Unavailable');},dispose(){disposed++;}})}];
const host=createModuleHost({container,navigation,context},definitions);
await host.sync();assert.equal(loaded,1);assert.equal(disposed,1);assert.equal(container.children.length,2);
assert.equal(container.children[0].hidden,true);assert.equal(container.children[1].hidden,false);
assert.equal(navigation.children[1].attributes['aria-current'],'page');
const stablePanel=container.children[0];stablePanel.draft='unsaved';
const click={button:0,preventDefault(){}};
navigation.children[0].events.click(click);
assert.equal(window.location.hash,'#section-stable');assert.equal(stablePanel.hidden,false);assert.equal(container.children[1].hidden,true);
window.location.hash='#section-optional';browserEvents.get('popstate')();
assert.equal(stablePanel.hidden,true);assert.equal(container.children[1].hidden,false);
fail=false;await container.children[1].children[0].children[2].events.click();
assert.equal(loaded,1);assert.equal(container.children[0],stablePanel);assert.equal(stablePanel.draft,'unsaved');
enabled=false;await host.sync();assert.equal(container.children.length,1);assert.equal(loaded,1);assert.ok(updated>0);
assert.equal(window.location.hash,'#section-stable');assert.equal(stablePanel.hidden,false);
window.location.hash='#section-forbidden';browserEvents.get('hashchange')();
assert.equal(window.location.hash,'#section-stable');assert.equal(stablePanel.hidden,false);
profile={user:{id:'2',role:'customer'}};await host.sync();assert.equal(loaded,2);assert.notEqual(container.children[0],stablePanel);
host.dispose();assert.equal(container.children.length,0);assert.equal(navigation.children.length,0);
assert.equal(browserEvents.size,0);
let release, waitingDisposed=0;
const delayed=createModuleHost({container,navigation,context},[{id:'delayed',title:'profile',enabled:()=>true,
 create:()=>new Promise(resolve=>{release=()=>resolve({load(){throw new Error('Stale module loaded');},dispose(){waitingDisposed++;}});})}]);
const pending=delayed.sync();delayed.dispose();release();await pending;
assert.equal(waitingDisposed,1);assert.equal(container.children.length,0);
let finishLoad, entered;
const entering=new Promise(resolve=>{entered=resolve;});
const slow=createModuleHost({container,navigation,context},[
 {id:'first',title:'profile',enabled:()=>true,create:async()=>({load(){},dispose(){}})},
 {id:'slow',title:'users',enabled:()=>true,create:()=>new Promise(resolve=>{
   finishLoad=()=>resolve({load(){},dispose(){}});entered();
 })}
]);
window.location.hash='#section-first';const loading=slow.sync();await entering;
window.location.hash='#section-slow';browserEvents.get('popstate')();
assert.equal(window.location.hash,'#section-slow');assert.ok(container.children.every(panel=>panel.hidden));
finishLoad();await loading;
assert.deepEqual(container.children.filter(panel=>!panel.hidden).map(panel=>panel.id),['section-slow']);
slow.dispose();
const denied=createModuleHost({container,navigation,context},[{id:'denied',title:'users',enabled:()=>true,
 create:async()=>{throw Object.assign(new Error('Signed out'),{status:401});}}]);
await assert.rejects(denied.sync(),error=>error.status===401);denied.dispose();
assert.equal(container.children.length,0);assert.equal(navigation.children.length,0);
// Re-evaluate the actual role predicates even when a user's ID stays the same.
context.features.reservations=true;context.isAdminPage=false;profile.user.role='customer';
let removedReservation=0;
const roles=createModuleHost({container,navigation,context},accountModules.map(definition=>({...definition,
  create:async()=>({load(){},dispose(){if(definition.id==='reservations') removedReservation++;}})
})));
window.location.hash='#section-reservations';await roles.sync();
assert.deepEqual(container.children.map(panel=>panel.id),['section-reservations','section-profile']);
profile.user.role='operator';await roles.sync();
assert.equal(removedReservation,1);assert.equal(window.location.hash,'#section-profile');
assert.deepEqual(container.children.map(panel=>panel.id),['section-profile']);
context.isAdminPage=true;await roles.sync();
assert.deepEqual(container.children.map(panel=>panel.id),['section-reservations','section-profile']);
profile={user:{id:'another-customer',role:'customer'}};await roles.sync();
assert.equal(container.children.length,0);assert.equal(navigation.children.length,0);roles.dispose();
context.isAdminPage=true;profile.user.role='admin';context.options.email=true;
assert.ok(visible().includes('mail'));
profile.user.role='operator';assert.ok(!visible().includes('mail'));
profile.user.role='customer';assert.ok(!visible().includes('mail'));
profile.user.role='admin';context.options.email=false;assert.ok(!visible().includes('mail'));

const i18nUrl='data:text/javascript;base64,'+Buffer.from(await readFile('Admin/i18n.js')).toString('base64');
const mailSource=(await readFile('Admin/mail.js','utf8')).replace('/accounts-assets/i18n.js',i18nUrl);
const {createMail}=await import('data:text/javascript;base64,'+Buffer.from(mailSource).toString('base64'));
const mailContainer=new Node('section'), requests=[];
let mailResponse={transport:'local_outbox',worker:{healthy:true,heartbeatAt:'2026-09-11T08:00:00Z',error:'',paused:false},
 counts:{outbox:'1'},nextBefore:'5',jobs:[{id:'6',kind:'notice',subject:'<script>literal</script>',recipient:'person@example.test',
 state:'delivered',transport:'local_outbox',attempts:'1',createdAt:'2026-09-11T08:00:00Z',error:''}]};
const mail=createMail({container:mailContainer,language:'en',run:fn=>fn(),api:{request:async path=>{requests.push(path);return mailResponse;}}});
const contents=node=>[node.textContent||'',...node.children.map(contents)].join(' ');
await mail.load();
assert.ok(contents(mailContainer).includes('Written to local outbox'));
assert.ok(contents(mailContainer).includes('<script>literal</script>'));
assert.equal(requests[0],'/api/v1/admin/mail');
const mailControls=mailContainer.children[2], mailFilter=mailControls.children[0].children[0];
mailFilter.value='failed';await mailFilter.events.change();
assert.equal(requests.at(-1),'/api/v1/admin/mail?state=failed');
await mailControls.children[2].events.click();
assert.equal(requests.at(-1),'/api/v1/admin/mail?before=5&state=failed');
mailResponse={...mailResponse,worker:{...mailResponse.worker,healthy:false,error:'reminder_scheduler_failed'},jobs:[],nextBefore:''};
await mailControls.children[1].events.click();
assert.ok(contents(mailContainer).includes('Worker has not checked in recently'));
assert.ok(contents(mailContainer).includes('Reminders could not be scheduled'));
assert.ok(mailControls.children[2].disabled);
assert.equal(requests.at(-1),'/api/v1/admin/mail?state=failed');
let releaseMail;mailResponse=new Promise(resolve=>{releaseMail=resolve;});
const pendingMail=mail.load();mail.dispose();releaseMail({});await pendingMail;
assert.equal(mailContainer.children.length,0);
console.log('PASS: role-based navigation, identity labels, sections, draft preservation and teardown; email panel status labels, filters, paging and safe text');
''', text=True)


def check_public_accounts():
    """Exercise the actual shared controller and AccountApi with delayed responses."""
    subprocess.run(['node', '--input-type=module', '-'], cwd=ROOT, check=True, input=r'''
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const sources = {};
for (const name of ['api.js', 'i18n.js', 'modules.js']) {
  sources[name] = 'data:text/javascript;base64,' + Buffer.from(await readFile('Admin/' + name)).toString('base64');
}
const source = (await readFile('Admin/public-account.js', 'utf8'))
  .replace(/"\/accounts-assets\/([^"]+)"/g, (_match, name) => JSON.stringify(sources[name]));
const { mountPublicAccount } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
class Node extends EventTarget {
  children = []; attributes = {}; hidden = false; value = ''; open = false;
  constructor(tag) { super(); this.tag = tag; }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this.children = nodes; }
  setAttribute(name, value) { this.attributes[name] = value; }
  showModal() { this.open = true; }
  close() { this.open = false; this.dispatchEvent(new Event('close')); }
  focus() {}
}
globalThis.document = { createElement: tag => new Node(tag) };
globalThis.window = new EventTarget();
globalThis.location = { search: '', assign() { throw new Error('Unexpected redirect'); } };
const user = role => ({ id: role, role, email: role + '@example.test', displayName: '<b>Literal name</b>' });
const response = (body, status = 200) => ({ ok: status === 200, status, json: async () => body });
let nextMe, loginRole = 'operator', loginStatus = 200, calls = [];
globalThis.fetch = async (path, request) => {
  calls.push({ path, ...request });
  if (path === '/api/v1/auth/options') return response({ ui: { defaultLocale: 'en' }, registration: true, email: true });
  if (path === '/api/v1/me') return nextMe ? nextMe : response({ user: user('customer'), csrfToken: 'initial' });
  if (path === '/api/v1/auth/login') return response(loginStatus === 200 ? { user: user(loginRole), csrfToken: 'new-session' } : { error: 'Invalid login' }, loginStatus);
  if (path === '/api/v1/auth/logout') return response({});
  throw new Error('Unexpected request: ' + path);
};
const navigation = new Node('nav'), dialog = new Node('dialog'), profileLink = new Node('a');
dialog.id = 'shop-login';
const settings = { navigation, dialog, profileLink, features: { accounts: false, reservations: true }, language: 'fr' };
assert.equal(await mountPublicAccount(settings), null);
assert.equal(calls.length, 0);
settings.features.accounts = true;
const controls = await mountPublicAccount(settings);
const hasLink = path => navigation.children.some(node => node.href?.includes(path));
assert.equal(controls.language, 'en');
assert.equal(dialog.attributes['aria-labelledby'], 'shop-login-title');
assert.equal(hasLink('/admin'), false);
assert.equal(hasLink('#section-reservations'), true);
assert.ok(navigation.children[0].textContent.includes('<b>Literal name</b>'));
assert.equal(navigation.children[0].children.length, 0); // Identity remains plain text.
assert.ok(profileLink.href.endsWith('#section-profile'));
let release;
nextMe = new Promise(resolve => { release = resolve; });
const pendingRead = controls.refresh();
controls.email.value = 'operator@example.test'; controls.password.value = 'test password';
await controls.login();
assert.equal(controls.password.value, '');
release(response({ user: user('customer'), csrfToken: 'stale' }));
await pendingRead;
assert.equal(controls.user.role, 'operator');
assert.equal(controls.api.csrfToken, 'new-session');
assert.equal(hasLink('/admin'), true);
assert.equal(navigation.children.some(node => node.href?.includes('/profile') && node.href.includes('reservations')), false);
controls.setLanguage('bg');
assert.ok(navigation.children[0].textContent.startsWith('Влезли сте като:'));
loginRole = 'admin'; await controls.login();
assert.equal(hasLink('/admin'), true);
assert.equal(navigation.children.some(node => node.href?.includes('/profile') && node.href.includes('reservations')), false);
nextMe = new Promise(resolve => { release = resolve; });
const beforeLogout = controls.refresh();
await controls.logout();
assert.equal(calls.findLast(call => call.path.endsWith('/logout')).headers['X-CSRF-Token'], 'new-session');
release(response({ user: user('admin'), csrfToken: 'stale-again' }));
await beforeLogout;
assert.equal(controls.user, null);
assert.equal(navigation.children.length, 1);
assert.equal(navigation.children[0].textContent, 'Вход');
loginStatus = 401; controls.password.value = 'wrong'; await controls.login();
assert.equal(controls.password.value, ''); assert.equal(controls.status.textContent, 'Invalid login');
assert.equal(controls.user, null);
nextMe = new Promise(resolve => { release = resolve; });
const beforeDispose = controls.refresh();
controls.dispose();
release(response({ user: user('customer'), csrfToken: 'after-dispose' }));
await beforeDispose;
assert.equal(navigation.children.length, 0); assert.equal(navigation.hidden, true);
const count = calls.length;
window.dispatchEvent(new Event('focus'));
assert.equal(calls.length, count);
console.log('PASS: reusable public login, role navigation, language fallback, text safety, stale-session responses and teardown');
''', text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'Build/WebSiteBackend')
    parser.add_argument('--public-binary', type=Path, default=ROOT / 'Build/PublicOnly/WebSiteBackend')
    parser.add_argument('--content-editor', action='store_true', help='Also exercise service publishing against a temporary content copy')
    parser.add_argument('--customer-email', action='store_true', help='Also exercise customer registration, recovery and newsletters using a private test outbox')
    parser.add_argument('--email-delivery', action='store_true', help='Exercise the Resend transport with an isolated loopback provider; requires --customer-email')
    parser.add_argument('--reservations', action='store_true', help='Exercise optional reservations against disposable schedules and accounts')
    parser.add_argument('--diagnose-timeouts', action='store_true', help='Use installed gdb to capture test-server stacks after a timeout')
    args = parser.parse_args()
    if args.email_delivery and not args.customer_email:
        parser.error('--email-delivery requires --customer-email')
    check_admin_modules()
    check_public_accounts()
    pg, env = binaries()
    processes = []
    with tempfile.TemporaryDirectory(prefix='accounts-test-', dir=ROOT / 'Build') as directory:
        work = Path(directory)
        socket = work / 'socket'
        socket.mkdir(mode=0o700)
        data_dir = work / 'pgdata'
        with (work / 'process.log').open('w') as log:
            def run(command, **kwargs):
                return subprocess.run([str(x) for x in command], env=env, check=True, stdout=log, stderr=log, **kwargs)

            def sql(statement):
                return subprocess.check_output([str(pg / 'psql'), '-X', '-h', str(socket), '-p', '55432', '-U', 'accounts_test', '-d', 'postgres', '-At', '-v', 'ON_ERROR_STOP=1', '-c', statement], env=env, text=True).strip()

            def start(config, binary=args.binary):
                process = subprocess.Popen([str(binary), '--config=' + str(config)], env=env, stdout=log, stderr=log)
                processes.append(process)
                port = json.loads(config.read_text())['listeners'][0]['port']
                for _ in range(100):
                    assert process.poll() is None, (work / 'process.log').read_text()[-3000:]
                    try:
                        Client(port).request('/health')
                        return process
                    except OSError:
                        time.sleep(.05)
                raise AssertionError('Server readiness timed out')

            def stop(process):
                process.terminate()
                assert process.wait(timeout=12) == 0

            def config_file(name, port=18082, enabled=True, origin=None):
                config = json.loads((EXAMPLE / 'Config/server-accounts.json').read_text())
                config['listeners'][0]['port'] = port
                config['app']['document_root'] = str(EXAMPLE / 'Public')
                config['app']['upload_path'] = str(work / ('uploads-' + name))
                # Every server gets its own content file, including concurrently
                # running HTTPS/disabled fixtures. Never publish into the example.
                content = work / (name + '.content.json')
                shutil.copyfile(EXAMPLE / 'Content/site.json', content)
                config['custom_config']['public_content_file'] = str(content)
                config['custom_config']['content_editor'] = {
                    'enabled': args.content_editor and enabled,
                    'backups_directory': str(work / (name + '.backups')),
                    'currencies': ['EUR']
                }
                config['custom_config']['reservations']['enabled'] = args.reservations and enabled
                config['custom_config']['reservations']['schema_file'] = str(ROOT/'Migrations/Reservations/001_reservations.sql')
                accounts = config['custom_config']['accounts']
                accounts.update(enabled=enabled, ui_root=str(ROOT/'Admin'), schema_file=str(ROOT/'Migrations/001_accounts.sql'), public_origin=origin or f'http://127.0.0.1:{port}')
                accounts['customer_email'].update(enabled=args.customer_email and enabled, outbox_directory=str(work / (name + '.mail')))
                target = work / name
                target.write_text(json.dumps(config))
                return target

            def command(config, operation, password=None, success=True):
                cmd = [str(args.binary), '--config=' + str(config), operation]
                if password is not None:
                    cmd.append('--password-stdin')
                result = subprocess.run(cmd, env=env, input=(password+'\n').encode() if password else None, stdout=log, stderr=log, timeout=12)
                assert (result.returncode == 0) == success, (operation, result.returncode, (work / 'process.log').read_text()[-1500:])

            try:
                run([pg/'initdb', '-D', data_dir, '-U', 'accounts_test', '--auth-local=trust', '--auth-host=reject', '--no-locale', '--encoding=UTF8'])
                postgres = subprocess.Popen([str(pg/'postgres'), '-D', str(data_dir), '-F', '-h', '', '-k', str(socket), '-p', '55432'], env=env, stdout=log, stderr=log)
                processes.append(postgres)
                for _ in range(100):
                    if (socket / '.s.PGSQL.55432').exists():
                        ready = subprocess.run([str(pg/'pg_isready'), '-h', str(socket), '-p', '55432'], env=env, stdout=log, stderr=log)
                        if ready.returncode == 0:
                            break
                    time.sleep(.05)
                env['ALOHA_DATABASE_URL'] = f'host={socket} port=55432 dbname=postgres user=accounts_test'
                config = config_file('accounts.json')
                command(config, '--migrate-accounts')
                command(config, '--migrate-accounts')
                if args.reservations:
                    command(config, '--migrate-reservations')
                    command(config, '--migrate-reservations')
                password = secrets.token_urlsafe(24)
                command(config, '--bootstrap-admin=admin@example.test', password)
                command(config, '--bootstrap-admin=second@example.test', password, success=False)
                if args.customer_email:
                    # Upgrade a separate version-1 database containing real credentials
                    # and a session, without downgrading or altering the active fixture.
                    sql('CREATE DATABASE legacy_upgrade')
                    def legacy_sql(statement):
                        return subprocess.check_output([str(pg/'psql'), '-X', '-h', str(socket), '-p', '55432', '-U', 'accounts_test', '-d', 'legacy_upgrade', '-At', '-v', 'ON_ERROR_STOP=1', '-c', statement], env=env, text=True).strip()
                    legacy_sql((ROOT/'Migrations/001_accounts.sql').read_text())
                    encoded = sql("SELECT password_hash FROM accounts.users WHERE email='admin@example.test'")
                    legacy_sql("INSERT INTO accounts.users(email,password_hash,display_name,locale,role) VALUES ('legacy@example.test','" + encoded.replace("'", "''") + "','Legacy','bg','admin')")
                    legacy_sql("INSERT INTO accounts.sessions(token_hash,user_id,csrf_token,expires_at) VALUES (repeat('a',64),1,repeat('b',64),now()+interval '1 hour')")
                    before = legacy_sql('SELECT id,email,password_hash,display_name,locale,role FROM accounts.users')
                    sessions = legacy_sql('SELECT * FROM accounts.sessions')
                    connection = env['ALOHA_DATABASE_URL']
                    try:
                        env['ALOHA_DATABASE_URL'] = connection.replace('dbname=postgres', 'dbname=legacy_upgrade')
                        command(config, '--migrate-accounts')
                        command(config, '--migrate-accounts')
                    finally:
                        env['ALOHA_DATABASE_URL'] = connection
                    assert before == legacy_sql('SELECT id,email,password_hash,display_name,locale,role FROM accounts.users')
                    assert sessions == legacy_sql('SELECT * FROM accounts.sessions')
                    assert legacy_sql('SELECT email_verified_at IS NULL FROM accounts.users') == 't'
                    assert legacy_sql('SELECT version FROM accounts.schema_version') == '4'
                    assert legacy_sql('SELECT last_login_at IS NULL FROM accounts.users') == 't'
                    print('PASS: version-1 upgrade preserves credentials, roles and sessions without silently verifying email', flush=True)
                    # Version 2 is the schema deployed before activity tracking.
                    sql('CREATE DATABASE activity_upgrade')
                    def activity_sql(statement):
                        return subprocess.check_output([str(pg/'psql'), '-X', '-h', str(socket), '-p', '55432', '-U', 'accounts_test', '-d', 'activity_upgrade', '-At', '-v', 'ON_ERROR_STOP=1', '-c', statement], env=env, text=True).strip()
                    activity_sql((ROOT/'Migrations/001_accounts.sql').read_text())
                    activity_sql((ROOT/'Migrations/002_customer_email.sql').read_text())
                    activity_sql("INSERT INTO accounts.users(email,password_hash,display_name,locale,role,email_verified_at) VALUES ('existing@example.test','" + encoded.replace("'", "''") + "','Existing','en','admin',now()),('unknown@example.test',NULL,'Unknown','en','customer',NULL)")
                    activity_sql("INSERT INTO accounts.audit(actor_id,subject_id,action,occurred_at) VALUES (1,1,'login','2026-01-01Z'),(1,1,'login','2026-02-01Z'),(1,1,'profile.updated','2026-03-01Z')")
                    activity_sql("INSERT INTO accounts.sessions(token_hash,user_id,csrf_token,expires_at) VALUES (repeat('c',64),1,repeat('d',64),now()+interval '1 hour')")
                    before = activity_sql('SELECT id,email,password_hash,version,created_at,email_verified_at FROM accounts.users ORDER BY id')
                    sessions = activity_sql('SELECT * FROM accounts.sessions')
                    try:
                        env['ALOHA_DATABASE_URL'] = connection.replace('dbname=postgres', 'dbname=activity_upgrade')
                        command(config, '--migrate-accounts')
                        command(config, '--migrate-accounts')
                    finally:
                        env['ALOHA_DATABASE_URL'] = connection
                    assert before == activity_sql('SELECT id,email,password_hash,version,created_at,email_verified_at FROM accounts.users ORDER BY id')
                    assert sessions == activity_sql('SELECT * FROM accounts.sessions')
                    assert activity_sql("SELECT last_login_at='2026-02-01Z'::timestamptz FROM accounts.users WHERE id=1") == 't'
                    assert activity_sql('SELECT last_login_at IS NULL FROM accounts.users WHERE id=2') == 't'
                    assert activity_sql('SELECT count(*) FROM accounts.audit') == '3'
                    assert activity_sql('SELECT version FROM accounts.schema_version') == '4'
                    print('PASS: version-2 migration preserves accounts/sessions and backfills only successful login history', flush=True)
                server = start(config)
                if args.customer_email:
                    from customer_email_checks import check_customer_email
                    def restart():
                        nonlocal server
                        stop(server)
                        server = start(config)
                    check_customer_email(Client, password, sql, config, restart, command)
                    if args.email_delivery:
                        from email_delivery_checks import check_email_delivery
                        check_email_delivery(Client, password, sql, config, restart, command, env)
                    restart()  # Independent baseline checks get fresh HTTP rate-limit windows.
                anonymous = Client()
                assert anonymous.request('/api/v1/features')[0] == {'accounts': True, 'contentEditor': args.content_editor, 'reservations': args.reservations}
                anonymous.request('/api/v1/me', expected=401)
                anonymous.request('/api/v1/admin/users', expected=401)
                for path, method in [('/api/v1/me','PATCH'),('/api/v1/admin/users','POST'),('/api/v1/admin/users','PATCH')]:
                    anonymous.request(path, method, {}, expected=401)
                anonymous.request('/api/v1/auth/register', 'POST', {}, expected=400 if args.customer_email else 404)
                if not args.customer_email:
                    assert not anonymous.request('/api/v1/auth/options')[0]['email']
                    for path in ['/email','/accounts-assets/email.js','/api/v1/admin/newsletters']:
                        anonymous.request(path, expected=404)
                unknown, _ = anonymous.login('missing@example.test', password, expected=401)
                wrong, _ = anonymous.login('admin@example.test', 'wrong password long enough', expected=401)
                assert unknown == wrong
                anonymous.request('/api/v1/auth/login', 'POST', {'email':'admin@example.test','password':password}, expected=403, headers={'Origin':'https://evil.example'})
                anonymous.request('/api/v1/auth/login', 'POST', {}, expected=415, headers={'Content-Type':'text/plain'})
                anonymous.request('/api/v1/auth/login', 'POST', expected=400, raw=b'{"email":"a@example.test","email":"b@example.test"}')
                print('PASS: migrations, bootstrap, anonymous access, generic login errors, origin and strict JSON checks', flush=True)

                admin = Client()
                _, headers = admin.login('ADMIN@example.test', password)
                cookie = next(v for k,v in headers.items() if k.lower() == 'set-cookie')
                assert 'HttpOnly' in cookie and 'SameSite=Lax' in cookie and 'Path=/' in cookie and 'Domain=' not in cookie
                me = admin.request('/api/v1/me')[0]
                activity = admin.request('/api/v1/admin/users?q=admin%40example.test')[0]['users'][0]
                assert activity['createdAt'].endswith('Z') and activity['lastLoginAt'].endswith('Z')
                login_time = activity['lastLoginAt']
                anonymous.login('admin@example.test', 'wrong password long enough', expected=401)
                admin.request('/api/v1/me')
                assert admin.request('/api/v1/admin/users?q=admin%40example.test')[0]['users'][0]['lastLoginAt'] == login_time
                login_check = Client(); login_check.login('admin@example.test', password)
                assert admin.request('/api/v1/admin/users?q=admin%40example.test')[0]['users'][0]['lastLoginAt'] > login_time
                assert admin.request('/api/v1/me')[0]['user']['version'] == me['user']['version']
                assert me['user']['role'] == 'admin' and me['locales'] == ['bg','en']
                assert 'password' not in json.dumps(me).lower()
                profile = {'displayName':'Администратор 🌺','phone':'+359 123','locale':'bg','version':me['user']['version']}
                session_query = "SELECT token_hash,last_seen FROM accounts.sessions WHERE user_id=" + me['user']['id'] + " ORDER BY token_hash"
                sql("UPDATE accounts.sessions SET last_seen=now()-interval '1 second' WHERE user_id=" + me['user']['id'])
                session_before = sql(session_query)
                session_cookie = admin.cookie
                for csrf in ['', 'bad', 'z'*64]:
                    rejected, _ = admin.request('/api/v1/me', 'PATCH', profile, expected=403, headers={'X-CSRF-Token':csrf})
                    assert rejected == {'error':'Invalid request token'}
                    assert admin.cookie == session_cookie and sql(session_query) == session_before
                probe = Client()
                cookie_name = session_cookie.split('=', 1)[0]
                for invalid_token in ['short', 'z'*64]:
                    probe.cookie = cookie_name + '=' + invalid_token
                    rejected, _ = probe.request('/api/v1/me', 'PATCH', profile, expected=401, headers={'X-CSRF-Token':'z'*64})
                    assert rejected == {'error':'Please sign in'} and probe.cookie == ''
                    assert sql(session_query) == session_before
                # Reads do not require CSRF, and a successful lookup refreshes activity.
                read, _ = admin.request('/api/v1/me', headers={'X-CSRF-Token':'z'*64})
                assert read == me and sql(session_query) != session_before
                print('PASS: session lookup, mutation-only CSRF, rejected-request activity rollback and read refresh', flush=True)
                admin.request('/api/v1/me', 'PATCH', {**profile,'role':'admin'}, expected=400)
                profile_id = me['user']['id']
                profile_credential = sql("SELECT credential_version FROM accounts.users WHERE id=" + profile_id)
                profile_audits = int(sql("SELECT count(*) FROM accounts.audit WHERE subject_id=" + profile_id + " AND action='profile.updated'"))
                updated, _ = admin.request('/api/v1/me', 'PATCH', profile)
                assert updated == {}
                refreshed = admin.request('/api/v1/me')[0]
                assert refreshed['user']['displayName'] == 'Администратор 🌺'
                assert refreshed['user']['version'] == str(int(profile['version']) + 1)
                assert refreshed['csrfToken'] == me['csrfToken']
                assert sql("SELECT credential_version FROM accounts.users WHERE id=" + profile_id) == profile_credential
                profile_sessions = sql("SELECT token_hash,last_seen FROM accounts.sessions WHERE user_id=" + profile_id + " ORDER BY token_hash")
                admin.request('/api/v1/me', 'PATCH', profile, expected=409)
                assert sql("SELECT token_hash,last_seen FROM accounts.sessions WHERE user_id=" + profile_id + " ORDER BY token_hash") == profile_sessions
                assert admin.request('/api/v1/me')[0]['user']['version'] == refreshed['user']['version']
                assert int(sql("SELECT count(*) FROM accounts.audit WHERE subject_id=" + profile_id + " AND action='profile.updated'")) == profile_audits + 1
                other_password = secrets.token_urlsafe(24)
                ids = {}
                for role in ['customer','operator','admin']:
                    body = {'email':role+'2@example.test','password':other_password,'displayName':role,'phone':'','locale':'en','role':role}
                    ids[role] = admin.request('/api/v1/admin/users', 'POST', body, expected=201)[0]['id']
                admin.request('/api/v1/admin/users','POST',body,expected=409)
                for account_id in ids.values():
                    assert isinstance(account_id, str) and account_id.isdecimal()
                    assert sql("SELECT count(*) FROM accounts.audit WHERE subject_id=" + account_id + " AND action='user.created'") == '1'
                customer = Client(); customer.login('customer2@example.test',other_password)
                operator = Client(); operator.login('operator2@example.test',other_password)
                for client in [customer,operator]:
                    client.request('/api/v1/admin/users',expected=403)
                    client.request('/api/v1/admin/users?page=1&role=admin&sort=email',expected=403)
                    client.request('/api/v1/admin/users','PATCH',{},expected=403)
                own = customer.request('/api/v1/me')[0]['user']
                customer.request('/api/v1/me','PATCH',{'displayName':'Changed','phone':'','locale':'en','version':own['version'],'id':ids['admin']},expected=400)
                assert admin.request('/api/v1/me')[0]['user']['displayName'] == 'Администратор 🌺'
                print('PASS: session cookies, CSRF, Unicode profiles, stale edits, role and ownership boundaries', flush=True)

                if args.reservations:
                    from reservations_checks import check_reservations
                    def restart_reservations():
                        nonlocal server
                        stop(server)
                        server = start(config)
                    check_reservations(Client, admin, customer, operator, anonymous, sql, config, command, restart_reservations)
                else:
                    for route in ['/api/v1/reservations/options','/api/v1/me/reservations','/accounts-assets/reservations.js']:
                        anonymous.request(route,expected=404)

                if args.content_editor:
                    from content_editor_checks import check_content_editor
                    check_content_editor(admin, customer, operator, anonymous, config, command, args.binary, env, log)
                else:
                    anonymous.request('/api/v1/admin/content/services', expected=404)
                    anonymous.request('/accounts-assets/content.js', expected=404)

                users = admin.request('/api/v1/admin/users')[0]['users']
                operator_row = next(u for u in users if u['id']==ids['operator'])
                operator_id = ids['operator']
                operator_credential = int(sql("SELECT credential_version FROM accounts.users WHERE id=" + operator_id))
                operator_audits = int(sql("SELECT count(*) FROM accounts.audit WHERE subject_id=" + operator_id + " AND action='user.access_changed'"))
                # Seed consent to exercise the existing account-disable side effect even with email routes disabled.
                sql("INSERT INTO accounts.subscriptions(user_id,email,confirmed_at) VALUES(" + operator_id + ",'operator2@example.test',now()) ON CONFLICT(user_id) DO UPDATE SET confirmed_at=now()")
                generation = int(sql("SELECT generation FROM accounts.subscriptions WHERE user_id=" + operator_id))
                disable = {'id':operator_id,'role':'operator','enabled':False,'version':operator_row['version']}
                changed, _ = admin.request('/api/v1/admin/users','PATCH',disable)
                assert changed == {}
                assert sql("SELECT version FROM accounts.users WHERE id=" + operator_id) == str(int(operator_row['version']) + 1)
                assert sql("SELECT credential_version FROM accounts.users WHERE id=" + operator_id) == str(operator_credential + 1)
                assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + operator_id) == '0'
                assert sql("SELECT confirmed_at IS NULL FROM accounts.subscriptions WHERE user_id=" + operator_id) == 't'
                assert sql("SELECT generation FROM accounts.subscriptions WHERE user_id=" + operator_id) == str(generation + 1)
                assert sql("SELECT count(*) FROM accounts.subscription_events WHERE user_id=" + operator_id + " AND source='account-disabled-v1'") == '1'
                admin.request('/api/v1/admin/users','PATCH',disable,expected=409)
                assert sql("SELECT version FROM accounts.users WHERE id=" + operator_id) == str(int(operator_row['version']) + 1)
                assert sql("SELECT credential_version FROM accounts.users WHERE id=" + operator_id) == str(operator_credential + 1)
                assert int(sql("SELECT count(*) FROM accounts.audit WHERE subject_id=" + operator_id + " AND action='user.access_changed'")) == operator_audits + 1
                operator.request('/api/v1/me',expected=401)
                operator.login('operator2@example.test',other_password,expected=401)
                missing, _ = admin.request('/api/v1/admin/users','PATCH',{'id':'999999999999999999','role':'customer','enabled':True,'version':'invalid'},expected=404)
                assert missing == {'error':'Account not found'}
                self_row = next(u for u in users if u['email']=='admin@example.test')
                admin.request('/api/v1/admin/users','PATCH',{'id':self_row['id'],'role':'customer','enabled':True,'version':self_row['version']},expected=409)
                rejected, _ = admin.request('/api/v1/admin/users','PATCH',{'id':self_row['id'],'role':'customer','enabled':True,'version':'invalid'},expected=409)
                assert rejected == {'error':'You cannot disable or demote your own administrator account'}
                self_credential = int(sql("SELECT credential_version FROM accounts.users WHERE id=" + self_row['id']))
                # Keeping one's own administrator access still invalidates all sessions and clears the cookie.
                changed, _ = admin.request('/api/v1/admin/users','PATCH',{'id':self_row['id'],'role':'admin','enabled':True,'version':self_row['version']})
                assert changed == {} and admin.cookie == ''
                assert sql("SELECT version FROM accounts.users WHERE id=" + self_row['id']) == str(int(self_row['version']) + 1)
                assert sql("SELECT credential_version FROM accounts.users WHERE id=" + self_row['id']) == str(self_credential + 1)
                assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + self_row['id']) == '0'
                admin.request('/api/v1/me',expected=401)
                admin.login('admin@example.test',password)
                print('PASS: profile/admin write versioning, stale-write rollback, audit, validation ordering and access invalidation', flush=True)
                copy = Client(); copy.login('customer2@example.test',other_password)
                new_password = secrets.token_urlsafe(24)
                password_query = "SELECT password_hash,version,credential_version,updated_at FROM accounts.users WHERE id=" + ids['customer']
                password_sessions_query = "SELECT * FROM accounts.sessions WHERE user_id=" + ids['customer'] + " ORDER BY token_hash"
                password_audit_query = "SELECT count(*) FROM accounts.audit WHERE subject_id=" + ids['customer'] + " AND action='password.changed'"
                before_password = sql(password_query)
                before_password_sessions = sql(password_sessions_query)
                password_audits = int(sql(password_audit_query))
                password_cookie = customer.cookie
                for body, status, error in [
                    ({'currentPassword':'wrong','newPassword':False,'extra':True},400,'Unexpected or missing fields'),
                    ({'currentPassword':False,'newPassword':'short'},400,'Invalid text field'),
                    ({'currentPassword':'not the right password','newPassword':False},403,'Current password is incorrect'),
                    ({'currentPassword':'not the right password','newPassword':new_password},403,'Current password is incorrect'),
                    ({'currentPassword':other_password,'newPassword':False},400,'Invalid text field'),
                    ({'currentPassword':other_password,'newPassword':'short'},400,'Text field length or encoding is invalid'),
                    ({'currentPassword':other_password,'newPassword':'a'*15+'\x7f'},400,'Text field length or encoding is invalid'),
                ]:
                    rejected, _ = customer.request('/api/v1/me/password','POST',body,expected=status)
                    assert rejected == {'error':error} and customer.cookie == password_cookie
                    assert sql(password_query) == before_password
                    assert sql(password_sessions_query) == before_password_sessions
                    assert int(sql(password_audit_query)) == password_audits
                old_hash, old_version, old_credential, _ = before_password.split('|')
                changed, _ = customer.request('/api/v1/me/password','POST',{'currentPassword':other_password,'newPassword':new_password})
                assert changed == {} and customer.cookie == ''
                changed_hash, changed_version, changed_credential, _ = sql(password_query).split('|')
                assert changed_hash != old_hash and changed_hash.startswith('$argon2id$v=19$m=19456,t=2,p=1$')
                assert int(changed_version) == int(old_version) + 1 and int(changed_credential) == int(old_credential) + 1
                assert sql(password_sessions_query) == ''
                assert int(sql(password_audit_query)) == password_audits + 1
                assert sql("SELECT actor_id FROM accounts.audit WHERE subject_id=" + ids['customer'] + " AND action='password.changed' ORDER BY id DESC LIMIT 1") == ids['customer']
                print('PASS: password validation ordering, rejected-change rollback, versioning, audit and cookie clearing', flush=True)
                customer.request('/api/v1/me',expected=401)
                copy.request('/api/v1/me',expected=401)
                customer.login('customer2@example.test',other_password,expected=401)
                customer.login('customer2@example.test',new_password)
                copy.login('customer2@example.test',new_password)
                token = customer.cookie
                logout_query = "SELECT token_hash,last_seen FROM accounts.sessions WHERE user_id=" + ids['customer'] + " ORDER BY token_hash"
                before_logout = sql(logout_query)
                assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + ids['customer']) == '2'
                customer.request('/api/v1/auth/logout','POST',{'unexpected':True},expected=400)
                assert customer.cookie == token and sql(logout_query) == before_logout
                logged_out, _ = customer.request('/api/v1/auth/logout','POST',{})
                assert logged_out == {} and customer.cookie == ''
                assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + ids['customer']) == '1'
                assert copy.request('/api/v1/me')[0]['user']['id'] == ids['customer']
                customer.cookie = token
                customer.request('/api/v1/me',expected=401)
                print('PASS: strict logout rolls back rejected requests and revokes only the current device', flush=True)
                customer.login('customer2@example.test',new_password)
                sql("UPDATE accounts.sessions SET last_seen=now()-interval '2 hours' WHERE user_id="+ids['customer'])
                customer.request('/api/v1/me',expected=401)
                customer.login('customer2@example.test',new_password)
                sql("UPDATE accounts.sessions SET expires_at=now()-interval '1 second' WHERE user_id="+ids['customer'])
                customer.request('/api/v1/me',expected=401)
                # Owner reset must revoke active devices too, not just the expired session above.
                customer.login('customer2@example.test',new_password)
                copy.login('customer2@example.test',new_password)
                before_reset = sql(password_query)
                before_reset_sessions = sql(password_sessions_query)
                reset_audit_query = "SELECT count(*) FROM accounts.audit WHERE action='password.reset_by_owner'"
                reset_audits = int(sql(reset_audit_query))
                for email, candidate, error in [
                    ('invalid','short','Invalid email address'),
                    ('customer2@example.test','short','Password must contain 15 to 128 characters without control characters'),
                    ('missing-reset@example.test','short','Password must contain 15 to 128 characters without control characters'),
                    ('missing-reset@example.test',other_password,'Account not found'),
                ]:
                    log_start = (work / 'process.log').stat().st_size
                    command(config,'--reset-account-password=' + email,candidate,success=False)
                    assert error in (work / 'process.log').read_bytes()[log_start:].decode()
                    assert sql(password_query) == before_reset and sql(password_sessions_query) == before_reset_sessions
                    assert int(sql(reset_audit_query)) == reset_audits
                assert sql("SELECT count(*) FROM accounts.sessions WHERE user_id=" + ids['customer']) == '2'
                command(config,'--reset-account-password=CUSTOMER2@EXAMPLE.TEST',other_password)
                reset_hash, reset_version, reset_credential, _ = sql(password_query).split('|')
                assert reset_hash != changed_hash and reset_hash.startswith('$argon2id$v=19$m=19456,t=2,p=1$')
                assert int(reset_version) == int(changed_version) + 1 and int(reset_credential) == int(changed_credential) + 1
                assert sql(password_sessions_query) == '' and int(sql(reset_audit_query)) == reset_audits + 1
                assert sql("SELECT actor_id IS NULL FROM accounts.audit WHERE subject_id=" + ids['customer'] + " AND action='password.reset_by_owner' ORDER BY id DESC LIMIT 1") == 't'
                customer.request('/api/v1/me',expected=401)
                copy.request('/api/v1/me',expected=401)
                customer.login('customer2@example.test',other_password)
                print('PASS: owner-reset validation ordering, missing accounts, normalized email, versioning, audit and active-device revocation', flush=True)
                print('PASS: disabling, self-admin protection, password verification, all-device revocation, logout, idle/absolute expiry and owner recovery', flush=True)

                for path in ['/admin','/profile','/accounts-assets/admin.js','/accounts-assets/api.js','/accounts-assets/public-account.js','/accounts-assets/admin.css']:
                    payload, headers = anonymous.request(path)
                    assert payload and any(k.lower()=='content-security-policy' and "frame-ancestors 'none'" in v for k,v in headers.items())
                for path in ['/accounts-assets/../Migrations/001_accounts.sql','/Config/server-accounts.json','/Admin/index.html']:
                    anonymous.request(path,expected=404)
                sql("INSERT INTO accounts.users(email,password_hash,display_name,locale,role) SELECT 'bulk' || n || '@example.test',password_hash,'Bulk','en','customer' FROM accounts.users CROSS JOIN generate_series(1,51) n WHERE email='admin@example.test'")
                listing = admin.request('/api/v1/admin/users')[0]
                assert len(listing['users']) == 50 and listing['nextCursor']
                following = admin.request('/api/v1/admin/users?after='+listing['nextCursor'])[0]
                assert following['users'] and not ({u['id'] for u in listing['users']} & {u['id'] for u in following['users']})
                def list_users(**query):
                    return admin.request('/api/v1/admin/users?' + urlencode({'page': 1, **query}))[0]
                bulk = [list_users(q='BULK', role='customer', enabled='true', sort='email', order='asc', page=p) for p in [1,2,3]]
                assert all(p['total'] == 51 and p['pages'] == 3 and p['pageSize'] == 25 for p in bulk)
                assert [len(p['users']) for p in bulk] == [25,25,1]
                bulk_rows = [u for p in bulk for u in p['users']]
                assert len({u['id'] for u in bulk_rows}) == 51
                emails = [u['email'] for u in bulk_rows]
                assert emails == sorted(emails)
                assert all(u['lastLoginAt'] is None and not u['emailVerified'] for u in bulk_rows)
                assert list_users(q='bulk', sort='email', order='desc')['users'][0]['email'] == emails[-1]
                assert list_users(q='bulk', page=1000000)['page'] == 3
                assert list_users(q='does-not-exist')['total'] == 0
                assert list_users(q='%')['total'] == 0  # Search is literal, not a SQL wildcard.
                assert list_users(q="' OR true --")['total'] == 0
                disabled = list_users(role='operator', enabled='false')
                assert disabled['total'] == 1 and disabled['users'][0]['id'] == ids['operator']
                assert list_users(q='Администратор')['users'][0]['email'] == 'admin@example.test'
                sql("UPDATE accounts.users SET created_at='2026-01-01Z',last_login_at='2026-02-01Z',email_verified_at=now() WHERE email='bulk1@example.test'")
                sql("UPDATE accounts.users SET created_at='2026-01-02Z',last_login_at='2026-03-01Z' WHERE email='bulk2@example.test'")
                assert list_users(q='bulk', sort='createdAt', order='asc')['users'][0]['email'] == 'bulk1@example.test'
                for order, first in [('asc','bulk1@example.test'),('desc','bulk2@example.test')]:
                    rows = list_users(q='bulk', sort='lastLoginAt', order=order)['users']
                    assert rows[0]['email'] == first and rows[2]['lastLoginAt'] is None
                assert list_users(q='bulk1@')['users'][0]['emailVerified']
                for query in ['page=0','page=-1','page=1000001','page=1x','page=','page=999999999999999999999','q=%00','q=%FF','q='+'a'*101,'role=owner','enabled=yes','sort=id','sort=email%3BSELECT+1','order=sideways','page=1&after=2']:
                    admin.request('/api/v1/admin/users?' + query, expected=400)
                print('PASS: global user search/filter/sort, deterministic pages, null activity, UTC dates, validation and legacy cursors', flush=True)
                assert sql("SELECT bool_and(password_hash LIKE '$argon2id$%') FROM accounts.users") == 't'
                assert sql("SELECT bool_and(length(token_hash)=64) FROM accounts.sessions") == 't'
                assert int(sql('SELECT count(*) FROM accounts.audit')) > 0
                stop(server); server = start(config)
                if args.content_editor:
                    source = Path(json.loads(config.read_text())['custom_config']['public_content_file'])
                    assert anonymous.request('/api/v1/site')[0] == json.loads(source.read_text())
                assert admin.request('/api/v1/me')[0]['user']['role']=='admin'
                with ThreadPoolExecutor(max_workers=8) as pool:
                    assert all(pool.map(lambda _: Client().request('/health')[0] == {'status':'ok'}, range(40)))
                print('PASS: admin assets/private paths, pagination, stored hashes, audit, persistent sessions and public health concurrency', flush=True)

                # Two administrators cannot concurrently remove each other's access.
                second = Client(); second.login('admin2@example.test',other_password)
                current_admin = admin.request('/api/v1/me')[0]['user']
                second_admin = second.request('/api/v1/me')[0]['user']
                def demote(client,target):
                    try:
                        client.request('/api/v1/admin/users','PATCH',{'id':target['id'],'role':'customer','enabled':True,'version':target['version']})
                        return True
                    except AssertionError as error:
                        assert error.args[0][2] in (401,403)
                        return False
                with ThreadPoolExecutor(max_workers=2) as pool:
                    results=list(pool.map(lambda pair: demote(*pair),[(admin,second_admin),(second,current_admin)]))
                assert sum(results)==1 and sql("SELECT count(*) FROM accounts.users WHERE role='admin' AND enabled")=='1'

                # A fresh identity must hit its per-account rate limit regardless of existence.
                limited = Client()
                for _ in range(10): limited.login('limited@example.test',password,expected=401)
                limited.login('limited@example.test',password,expected=429)
                secure_config = config_file('secure.json',18083,origin='https://accounts.example.test')
                secure = start(secure_config)
                secure_client = Client(18083,'https://accounts.example.test')
                _,secure_headers = secure_client.login('customer2@example.test',other_password)
                secure_cookie = next(v for k,v in secure_headers.items() if k.lower()=='set-cookie')
                assert '__Host-wsb-' in secure_cookie and 'Secure' in secure_cookie and 'Domain=' not in secure_cookie
                stop(secure)
                print('PASS: concurrent administrator changes, login throttling and HTTPS cookie policy', flush=True)

                survivor = admin if results[0] else second
                # PostgreSQL SIGTERM waits for clients; SIGINT disconnects them
                # and performs a clean fast shutdown, which exercises recovery.
                postgres.send_signal(signal.SIGINT)
                assert postgres.wait(timeout=12) == 0
                survivor.request('/api/v1/me',expected=503)
                Client().request('/health')
                postgres = subprocess.Popen([str(pg/'postgres'), '-D', str(data_dir), '-F', '-h', '', '-k', str(socket), '-p', '55432'],env=env,stdout=log,stderr=log)
                processes.append(postgres)
                for _ in range(100):
                    if subprocess.run([str(pg/'pg_isready'),'-h',str(socket),'-p','55432'],env=env,stdout=log,stderr=log).returncode == 0:
                        break
                    time.sleep(.05)
                # A worker with a stale connection may first detect the disconnect.
                for _ in range(4):
                    try:
                        survivor.request('/api/v1/me')
                        break
                    except AssertionError as error:
                        assert error.args[0][2] == 503
                else:
                    raise AssertionError('Database reconnect failed')
                print('PASS: database outage leaves public HTTP available and account workers reconnect',flush=True)

                # A second business uses the exact same admin assets with its own
                # configuration, database and content-language list.
                generic_config = config_file('generic.json', 18083)
                generic_data = json.loads(generic_config.read_text())
                generic_root = work/'generic-public'
                generic_root.mkdir()
                (generic_root/'index.html').write_text('<!doctype html><title>Corner Shop</title>')
                generic_data['app']['document_root'] = str(generic_root)
                generic_data['custom_config'].pop('public_content_file')
                generic_data['custom_config']['content_editor']['enabled'] = False
                generic_data['custom_config']['reservations']['enabled'] = False
                generic_accounts = generic_data['custom_config']['accounts']
                generic_accounts.update(database_url_env='GENERIC_DATABASE_URL', locales=['fr'])
                generic_accounts['customer_email']['footer'] = 'Corner Shop newsletter'
                generic_accounts['ui'] = {'site_name':'Corner Shop <literal>', 'default_locale':'en', 'support_text':{'en':'Contact shop support.'}}
                generic_config.write_text(json.dumps(generic_data))
                sql('CREATE DATABASE generic_site')
                env['GENERIC_DATABASE_URL'] = env['ALOHA_DATABASE_URL'].replace('dbname=postgres', 'dbname=generic_site')
                command(generic_config, '--migrate-accounts')
                command(generic_config, '--bootstrap-admin=shop-owner@example.test', password)
                generic_server = start(generic_config)
                generic_client = Client(18083)
                options = generic_client.request('/api/v1/auth/options')[0]
                assert options['ui'] == {'siteName':'Corner Shop <literal>', 'defaultLocale':'en', 'supportText':{'en':'Contact shop support.'}}
                assert options['locales'] == ['fr'] and set(options) == {'ui','email','registration','newsletters','transport','locales'}
                generic_client.cookie = survivor.cookie
                generic_client.request('/api/v1/me', expected=401)
                generic_client.login('admin@example.test', password, expected=401)
                generic_client.login('shop-owner@example.test', password)
                assert generic_client.request('/api/v1/me')[0]['user']['locale'] == 'fr'
                for name in ['admin.js','i18n.js','modules.js','profile.js','users.js','public-account.js']:
                    route = '/accounts-assets/' + name
                    assert generic_client.request(route)[0] == Client().request(route)[0]
                assert generic_client.request('/admin')[0] == Client().request('/admin')[0]
                assert Client().request('/api/v1/auth/options')[0]['ui']['siteName'] == 'Aloha Massage'
                generic_client.request('/api/v1/admin/content/services', expected=404)
                stop(generic_server)
                generic_accounts.pop('ui')
                generic_config.write_text(json.dumps(generic_data))
                generic_server = start(generic_config)
                assert generic_client.request('/api/v1/auth/options')[0]['ui'] == {'siteName':'','defaultLocale':'en','supportText':{}}
                stop(generic_server)
                for invalid_ui in [[], {'default_locale':'fr'}, {'site_name':'x'*101}, {'support_text':{'en':'bad\ntext'}}, {'support_text':{'fr':'unsupported'}}, {'script_url':'https://example.test/code.js'}]:
                    generic_accounts['ui'] = invalid_ui
                    generic_config.write_text(json.dumps(generic_data))
                    command(generic_config, '--migrate-accounts', success=False)
                env.pop('GENERIC_DATABASE_URL')
                print('PASS: shared admin assets across isolated sites, separate content languages, safe public settings and configuration defaults/validation', flush=True)

                off = config_file('disabled.json',18083,enabled=False)
                env.pop('ALOHA_DATABASE_URL')
                for binary in [args.binary,args.public_binary]:
                    public = start(off,binary)
                    client = Client(18083)
                    assert client.request('/api/v1/features')[0] == {'accounts':False, 'contentEditor':False, 'reservations':False}
                    client.request('/api/v1/site')
                    for path in ['/admin','/profile','/accounts-assets/admin.js','/accounts-assets/public-account.js','/accounts-assets/i18n.js','/accounts-assets/modules.js','/accounts-assets/profile.js','/accounts-assets/users.js','/api/v1/me','/api/v1/admin/users', '/api/v1/admin/content/services', '/accounts-assets/content.js']:
                        client.request(path,expected=404)
                    stop(public)
                rejected = subprocess.run([str(args.public_binary),'--config='+str(config)],env=env,stdout=log,stderr=log,timeout=8)
                assert rejected.returncode != 0
                print('PASS: runtime-disabled and compiled-out sites need no database and expose no account routes; incompatible configuration fails closed', flush=True)
            except Exception:
                # Capture bounded diagnostics before disposing of a failed fixture.
                # No locals/arguments are printed: test credentials stay private.
                if args.diagnose_timeouts and isinstance(sys.exc_info()[1], TimeoutError) and shutil.which('gdb'):
                    for process in processes:
                        if process.poll() is None and str(args.binary) in process.args:
                            for task in (Path('/proc') / str(process.pid) / 'task').iterdir():
                                print('Thread', task.name, 'wait:', (task / 'wchan').read_text(), flush=True)
                            try:
                                diagnostic = subprocess.run(['gdb', '-nx', '--batch', '-iex', 'set debuginfod enabled off', '-ex', 'set print frame-arguments none',
                                                '-ex', 'thread apply all bt 12', '-p', str(process.pid)],
                                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10, text=True)
                                print(diagnostic.stdout, flush=True)
                            except subprocess.TimeoutExpired as error:
                                print('Debugger timed out', error.stdout, flush=True)
                print((work/'process.log').read_text()[-4000:])
                raise
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try: process.wait(timeout=12)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait()


if __name__=='__main__':
    main()
