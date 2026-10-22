#!/usr/bin/env python3
#
# Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of manifold-run: it has to enter the mount tree the compose
service hands it, and run the program of the component in there.

The compose service is mocked by this test, as the real one can not mount
images in the sandbox. The tree it hands out is a bind of the host's /usr
below a tmpfs, with a shell script as the program of the component.

Both ways of entering a tree are exercised: directly, as root in the sandbox,
and in a user namespace of its own, by the unprivileged user running the
tests, outside of the sandbox. So the test runs in two halves:

  * the outer half, as the user, starts the inner half in the sandbox, waits
    for it to be ready, and runs the runner outside;
  * the inner half, as root in the sandbox, makes the tree, serves it, runs
    the runner inside, and then serves until the outer half is done.

They talk over a socket in a temporary directory of the host, which both see.

Usage: test-runner.py --sandbox mf-test-sandbox --runner manifold-run
"""

import argparse
import array
import ctypes
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import APP_ID, EXIT_SKIPPED, RUNTIME_DIR  # noqa: E402

TREE = f'{RUNTIME_DIR}/test-tree'
INTERFACE = 'org.freedesktop.manifold.run'

MS_BIND = 0x1000
MS_REC = 0x4000
SYS_OPEN_TREE = 428
OPEN_TREE_CLONE = 1
AT_RECURSIVE = 0x8000
AT_FDCWD = -100

PROBE = '''#!/bin/sh
echo "marker=$(cat /manifold-test-marker)"
echo "program=$0"
echo "arg=$1"
echo "cwd=$(pwd)"
echo "var=$MF_TEST_VAR"
echo "unset=${MF_TEST_UNSET-absent}"
echo "uid=$(id -u)"
'''

ARGS = None
libc = ctypes.CDLL(None, use_errno=True)


def mount(source, target, fstype, flags=0):
    def encode(value):
        return None if value is None else value.encode()

    if libc.mount(encode(source), encode(target), encode(fstype), flags, None) != 0:
        raise OSError(
            ctypes.get_errno(), f'Unable to mount {target}: {os.strerror(ctypes.get_errno())}'
        )


def open_tree(path):
    libc.syscall.restype = ctypes.c_int
    fd = libc.syscall(SYS_OPEN_TREE, AT_FDCWD, path.encode(), OPEN_TREE_CLONE | AT_RECURSIVE)
    if fd < 0:
        raise OSError(
            ctypes.get_errno(), f'Unable to clone {path}: {os.strerror(ctypes.get_errno())}'
        )
    return fd


def make_tree():
    """A root filesystem that can run a shell script, as the compose service would hand it out."""
    os.makedirs(TREE)
    mount('tmpfs', TREE, 'tmpfs')
    for name in ('usr', 'proc', 'app/bin', 'tmp', 'home'):
        os.makedirs(f'{TREE}/{name}')
    for name in ('bin', 'sbin', 'lib', 'lib64', 'lib32'):
        if os.path.islink(f'/{name}'):
            os.symlink(os.readlink(f'/{name}'), f'{TREE}/{name}')
    mount('/usr', f'{TREE}/usr', None, MS_BIND | MS_REC)
    mount('/proc', f'{TREE}/proc', None, MS_BIND | MS_REC)
    with open(f'{TREE}/manifold-test-marker', 'w', encoding='utf-8') as f:
        f.write('in the tree')
    with open(f'{TREE}/app/bin/probe', 'w', encoding='utf-8') as f:
        f.write(PROBE)
    os.chmod(f'{TREE}/app/bin/probe', 0o755)


class FakeService(threading.Thread):
    """Answers PrepareLaunch with a clone of the tree, like manifold-rund does."""

    def __init__(self, socket_path):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(socket_path)
        self.listener.listen()

    def run(self):
        while True:
            connection, _ = self.listener.accept()
            with connection:
                self.serve(connection)

    def serve(self, connection):
        buffer = b''
        while b'\0' not in buffer:
            chunk = connection.recv(65536)
            if not chunk:
                return
            buffer += chunk
        request = json.loads(buffer.split(b'\0', 1)[0])
        assert request['method'] == f'{INTERFACE}.PrepareLaunch', request
        component_id = request['parameters']['componentId']

        if component_id != APP_ID:
            reply = {
                'error': f'{INTERFACE}.NoSuchComponent',
                'parameters': {'componentId': component_id},
            }
            connection.sendall(json.dumps(reply).encode() + b'\0')
            return

        mount_fd = open_tree(TREE)
        reply = {
            'parameters': {
                'mountFileDescriptor': 0,
                'executable': '/app/bin/probe',
                'environment': ['MF_TEST_VAR=hello', 'MF_TEST_UNSET='],
            }
        }
        fds = array.array('i', [mount_fd])
        connection.sendmsg(
            [json.dumps(reply).encode() + b'\0'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)]
        )
        os.close(mount_fd)


class RunnerTest(unittest.TestCase):
    """The runner, as it is used where this half of the test runs."""

    socket_path = None
    uid = None  # the user the program in the tree runs as
    without_capabilities = False  # drop capabilities first, to test the user namespace path as root

    def run_runner(self, *args, cwd='/'):
        argv = [ARGS.runner, '--socket', self.socket_path, *args]
        if self.without_capabilities:
            argv = ['setpriv', '--bounding-set=-all', '--inh-caps=-all', *argv]
        environment = dict(os.environ, MF_TEST_UNSET='from the caller', HOME='/home')
        return subprocess.run(
            argv, capture_output=True, text=True, check=False, cwd=cwd, env=environment
        )

    def assert_launched(self, result, cwd):
        self.assertEqual(result.returncode, 0, result)
        lines = dict(line.split('=', 1) for line in result.stdout.splitlines())
        self.assertEqual(
            lines,
            {
                'marker': 'in the tree',
                'program': '/app/bin/probe',
                'arg': 'first',
                'cwd': cwd,
                'var': 'hello',
                'unset': 'absent',
                'uid': str(self.uid),
            },
            result.stderr,
        )

    def test_launch(self):
        self.assert_launched(self.run_runner(APP_ID, 'first', cwd='/usr/bin'), cwd='/usr/bin')

    def test_cwd_falls_back_to_home(self):
        """A directory the tree does not have falls back to the home directory."""
        self.assert_launched(self.run_runner(APP_ID, 'first', cwd='/run'), cwd='/home')

    def test_other_command(self):
        """Any program of the tree can be run instead of the one of the component."""
        result = self.run_runner(
            '--command=sh', APP_ID, '-c', 'echo "shell=$0"; cat /manifold-test-marker'
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertEqual(result.stdout, 'shell=sh\nin the tree')
        self.assert_launched(
            self.run_runner('--command=/app/bin/probe', APP_ID, 'first', cwd='/usr/bin'),
            cwd='/usr/bin',
        )
        result = self.run_runner('--command=no-such-program', APP_ID)
        self.assertEqual(result.returncode, 127, result)
        self.assertIn('no-such-program', result.stderr)

    def test_not_installed(self):
        result = self.run_runner('com.example.missing')
        self.assertEqual(result.returncode, 125, result)
        self.assertIn('Component com.example.missing is not installed', result.stderr)


def run_inside(socket_path):
    """The inner half: root in the sandbox, the runner enters the tree directly."""
    make_tree()
    FakeService(socket_path).start()
    RunnerTest.socket_path = socket_path
    RunnerTest.uid = 0
    result = unittest.main(argv=[sys.argv[0]], exit=False, verbosity=2).result
    if not result.wasSuccessful():
        return 1

    # serve until the outer half is done, which it tells us by closing our stdin
    print('ready', flush=True)
    sys.stdin.read()
    return 0


def run_outside():
    """The outer half: starts the inner half, and runs the runner outside of the sandbox."""
    work = tempfile.mkdtemp(prefix='manifold-test-')
    socket_path = os.path.join(work, 'run.sock')
    inside = subprocess.Popen(
        [ARGS.sandbox, sys.executable, __file__, '--inside', socket_path, *sys.argv[1:]],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        for line in inside.stdout:
            sys.stdout.write(line)
            if line.rstrip() == 'ready':
                break
        else:
            return inside.wait()

        # without capabilities, the runner creates a user namespace of its own
        if os.geteuid() == 0:
            if shutil.which('setpriv') is None:
                print('SKIP: running as root, and setpriv is not available to drop capabilities')
                return EXIT_SKIPPED
            RunnerTest.without_capabilities = True
        RunnerTest.socket_path = socket_path
        RunnerTest.uid = os.getuid()
        result = unittest.main(argv=[sys.argv[0]], exit=False, verbosity=2).result
    finally:
        inside.stdin.close()
        status = inside.wait()
        shutil.rmtree(work, ignore_errors=True)
    return 0 if result.wasSuccessful() and status == 0 else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--sandbox', required=True)
    parser.add_argument('--runner', required=True)
    parser.add_argument('--inside', metavar='SOCKET')
    ARGS = parser.parse_args()
    if ARGS.inside:
        os._exit(run_inside(ARGS.inside))  # the service thread must not keep us alive
    sys.exit(run_outside())
