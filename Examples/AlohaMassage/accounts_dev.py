#!/usr/bin/env python3
"""Local C++ accounts demo with a private PostgreSQL Unix socket. No system service."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

EXAMPLE = Path(__file__).resolve().parent
ROOT = EXAMPLE.parents[1]
RUNTIME = EXAMPLE / 'Runtime/Accounts'
CONFIG = EXAMPLE / 'Config/server-accounts.json'
BINARY = ROOT / 'Build/WebSiteBackend'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['start', 'bootstrap', 'reset-password'])
    parser.add_argument('email', nargs='?')
    args = parser.parse_args()
    if (args.command != 'start') != bool(args.email):
        parser.error('bootstrap and reset-password require an email; start does not')
    local = ROOT / 'Build/LocalPostgres/root'
    pg = local / 'usr/lib/postgresql/15/bin'
    if not (pg / 'postgres').exists():
        pg_config = shutil.which('pg_config')
        if not pg_config:
            parser.error('PostgreSQL binaries are missing; see Docs/ACCOUNTS.md')
        pg = Path(subprocess.check_output([pg_config, '--bindir'], text=True).strip())
    if not (pg / 'postgres').exists() or not BINARY.exists():
        parser.error('Build the accounts-enabled C++ executable and install PostgreSQL first')
    os.umask(0o077)
    RUNTIME.mkdir(parents=True, exist_ok=True, mode=0o700)
    RUNTIME.chmod(0o700)
    socket = RUNTIME / 'socket'
    socket.mkdir(exist_ok=True, mode=0o700)
    data = RUNTIME / 'pgdata'
    env = os.environ.copy()
    library = local / 'usr/lib/x86_64-linux-gnu'
    if library.exists():
        env['LD_LIBRARY_PATH'] = str(library) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    env['ALOHA_DATABASE_URL'] = f'host={socket} port=55432 dbname=aloha user=aloha_dev'
    postgres = None
    backend = None
    with (RUNTIME / 'postgres.log').open('a') as log:
        def run(command, **kwargs):
            return subprocess.run([str(x) for x in command], env=env, check=True, **kwargs)

        def sql(statement):
            return subprocess.check_output([str(pg/'psql'), '-X', '-h', str(socket), '-p', '55432', '-U', 'aloha_dev', '-d', 'postgres', '-At', '-v', 'ON_ERROR_STOP=1', '-c', statement], env=env, text=True).strip()

        try:
            if not (data / 'PG_VERSION').exists():
                run([pg/'initdb', '-D', data, '-U', 'aloha_dev', '--auth-local=trust', '--auth-host=reject', '--no-locale', '--encoding=UTF8'], stdout=log, stderr=log)
            ready_command = [str(pg/'pg_isready'), '-h', str(socket), '-p', '55432']
            if subprocess.run(ready_command, env=env, stdout=log, stderr=log).returncode != 0:
                postgres = subprocess.Popen([str(pg/'postgres'), '-D', str(data), '-h', '', '-k', str(socket), '-p', '55432'], env=env, stdout=log, stderr=log)
                for _ in range(100):
                    if postgres.poll() is not None:
                        raise RuntimeError('PostgreSQL startup failed; see Runtime/Accounts/postgres.log')
                    if subprocess.run(ready_command, env=env, stdout=log, stderr=log).returncode == 0:
                        break
                    time.sleep(.05)
                else:
                    raise RuntimeError('PostgreSQL startup timed out')
            if sql("SELECT 1 FROM pg_database WHERE datname='aloha'") != '1':
                sql('CREATE DATABASE aloha')
            run([BINARY, '--config='+str(CONFIG), '--migrate-accounts'])
            import json
            if json.loads(CONFIG.read_text())['custom_config'].get('reservations', {}).get('enabled'):
                run([BINARY, '--config='+str(CONFIG), '--migrate-reservations'])
            if args.command == 'start':
                backend = subprocess.Popen([str(BINARY), '--config='+str(CONFIG)], env=env)
                print('Accounts example: http://127.0.0.1:8082/profile?lang=bg', flush=True)
                print('Create your first admin from another terminal: python3 Examples/AlohaMassage/accounts_dev.py bootstrap YOUR_EMAIL', flush=True)
                backend.wait()
                if backend.returncode:
                    raise RuntimeError('The C++ backend exited with an error')
            else:
                command = '--bootstrap-admin=' if args.command == 'bootstrap' else '--reset-account-password='
                run([BINARY, '--config='+str(CONFIG), command+args.email])
        except KeyboardInterrupt:
            pass
        finally:
            # Only stop processes started by this invocation. A provisioning
            # command can safely reuse the demo's already running database.
            for process in (backend, postgres):
                if process is not None and process.poll() is None:
                    process.send_signal(signal.SIGINT if process is postgres else signal.SIGTERM)
                    try:
                        process.wait(timeout=12)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    main()
