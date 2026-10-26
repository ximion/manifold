#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
What the end-to-end tests share: how a daemon is started and stopped, how
it is called, and how bundles are made up. Calls are made with varlinkctl, and
with a client of our own where varlinkctl is too old to pass a file descriptor.

The tests run in the sandbox, which `mf-test-sandbox` enters for them: the
directories Manifold keeps its state in are empty, and we are root.
"""

import functools
import hashlib
import json
import os
import platform
import re
import signal
import socket
import subprocess
import sysconfig
import time
import zipfile

# the status Meson takes as "skipped"
EXIT_SKIPPED = 77

STORE = '/var/lib/manifold'
RUNTIME_DIR = '/run/manifold'

CORE_ID = 'net.debian.core13'
APP_ID = 'com.example.app'


def host_architecture():
    """The architecture of the host, as Manifold names it (mf_get_host_architecture).
    On x86 and 32-bit ARM, that is the architecture of the userland, which the multiarch
    tuple of Python tells. Elsewhere, it is the one of the kernel."""
    userland = {
        'x86_64-linux-gnu': 'x86_64',
        'i386-linux-gnu': 'i386',
        'arm-linux-gnueabihf': 'armhf',
        'arm-linux-gnueabi': 'armel',
    }.get(sysconfig.get_config_var('MULTIARCH'))
    if userland is not None:
        return userland
    machine = platform.machine()
    return 'i386' if re.fullmatch(r'i[3-6]86', machine) else machine


def fake_hash(seed):
    return hashlib.sha256(seed.encode()).hexdigest()


def blake3_hex(data):
    """The blake3 hash of data, which Python has no module for: b3sum computes it."""
    result = subprocess.run(['b3sum', '--no-names'], input=data, capture_output=True, check=True)
    return result.stdout.decode().strip()


@functools.cache
def varlinkctl_can_pass_fds():
    """Whether varlinkctl can pass a file descriptor along with a call, which it
    can since systemd 258."""
    result = subprocess.run(['varlinkctl', '--help'], capture_output=True, text=True, check=True)
    return '--push-fd=' in result.stdout


class VarlinkError(Exception):
    """A call was replied to with an error. The message names the error, and the
    parameters of the error are kept here."""

    def __init__(self, message, parameters):
        super().__init__(message)
        self.parameters = parameters


class Daemon:
    """A daemon of Manifold, listening on a socket of our choosing. It logs to a
    file: a pipe that is not read while the tests run would fill up, and block it."""

    def __init__(self, exe, socket_path, log_path):
        self.exe = exe
        self.socket_path = socket_path
        self.log_path = log_path
        self.process = None

    def start(self):
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        with open(self.log_path, 'wb') as log:
            self.process = subprocess.Popen(
                [self.exe, '--socket', self.socket_path, '--verbose'],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        for _ in range(200):
            if os.path.exists(self.socket_path):
                return
            if self.process.poll() is not None:
                raise RuntimeError(f'{self.exe} exited early:\n{self.read_log()}')
            time.sleep(0.05)
        raise RuntimeError(f'{self.exe} did not start listening')

    def log_size(self):
        return os.path.getsize(self.log_path)

    def read_log(self, start=0):
        """What the daemon logged, from the offset start on, e.g. a log_size() of before."""
        with open(self.log_path, 'rb') as f:
            f.seek(start)
            return f.read().decode(errors='replace')

    def stop(self):
        """Stop the daemon, and return what it logged. Stopping it twice is fine."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.process.returncode != 0:
            raise RuntimeError(
                f'{self.exe} exited with status {self.process.returncode}:\n{self.read_log()}'
            )
        return self.read_log()

    def call(self, method, parameters=None, more=False, fd=None):
        """
        Call a Varlink method, and return the parameters of the reply. With
        `more`, every reply is returned, as a list. A file descriptor is passed
        along as the first one, index 0. An error reply raises VarlinkError.
        """
        if fd is not None and not varlinkctl_can_pass_fds():
            return self._call_passing_fd(method, parameters, more, fd)

        argv = ['varlinkctl', 'call', '--json=short', '--timeout=infinity']
        if more:
            argv.append('--more')
        if fd is not None:
            argv.append(f'--push-fd={fd}')
        argv += [self.socket_path, method, json.dumps(parameters or {})]
        result = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            check=False,
            pass_fds=() if fd is None else (fd,),
        )
        # with `more`, the replies form a JSON text sequence: each one follows a record separator
        replies = [json.loads(reply) for reply in result.stdout.split('\x1e') if reply.strip()]
        if result.returncode != 0:
            raise VarlinkError(result.stderr.strip(), replies[0] if replies else {})
        return replies if more else replies[0]

    def _call_passing_fd(self, method, parameters, more, fd):
        """
        Make a call like varlinkctl does, for a varlinkctl that can't pass a file
        descriptor along. Varlink messages are JSON objects, each ended by a NUL
        byte, and the descriptor is sent with the message of the call.
        """
        call = {'method': method, 'parameters': parameters or {}}
        if more:
            call['more'] = True
        replies = []
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(self.socket_path)
            socket.send_fds(sock, [json.dumps(call).encode() + b'\0'], [fd])
            received = b''
            while True:
                while b'\0' not in received:
                    data = sock.recv(65536)
                    if not data:
                        raise RuntimeError(f'{self.exe} closed the connection without replying')
                    received += data
                message, _, received = received.partition(b'\0')
                reply = json.loads(message)
                if 'error' in reply:
                    raise VarlinkError(reply['error'], reply.get('parameters', {}))
                replies.append(reply.get('parameters', {}))
                # with `more`, every reply but the last one says that more follow
                if not reply.get('continues', False):
                    break
        return replies if more else replies[0]


class Image:
    """An image for a bundle, with its manifest. Its content is made up."""

    def __init__(
        self,
        component,
        kind,
        version,
        revision=22910000,
        sdk=False,
        base=None,
        seed=None,
        architecture=None,
    ):
        self.component = component
        self.kind = kind
        self.version = version
        self.revision = revision
        self.sdk = sdk
        self.base = base
        self.architecture = architecture or host_architecture()
        self.name = f'{component}_{version}.raw'
        self.identity = fake_hash('identity:' + component)
        self.data = hashlib.sha512((seed or self.name).encode()).digest() * 1024
        self.verity_hash = fake_hash('verity:' + hashlib.sha256(self.data).hexdigest())

    def reference(self):
        return {
            'component': self.component,
            'version': self.version,
            'revision': self.revision,
            'identity': self.identity,
            'content-hash': fake_hash('content:' + self.name),
        }

    def manifest(self):
        m = {
            'format': 1,
            'component': self.component,
            'kind': self.kind,
            'version': self.version,
            'revision': self.revision,
            'architecture': self.architecture,
            'sdk': self.sdk,
            'identity': self.identity,
            'content-hash': fake_hash('content:' + self.name),
            'image': {
                'file': self.name,
                'size': len(self.data),
                'sha256': hashlib.sha256(self.data).hexdigest(),
                'blake3': blake3_hex(self.data),
                'verity-root-hash': self.verity_hash,
                'data-offset': 20480,
                'hash-offset': 22528,
            },
        }
        if self.base is not None:
            m['base'] = self.base.reference()
        if self.kind == 'app' and not self.sdk:
            m['app'] = {'command': 'app'}
        return m


# the start of a JPEG XL codestream
JXL_DATA = b'\xff\x0afake'


def write_bundle(path, images, signed=True, corrupt_image=None, catalog_ids=None):
    """Write a .manifold file as the builder would, with a catalog that gives
    apps an icon and a screenshot. catalog_ids maps a component to the
    identifier its catalog component claims, when that is to differ."""
    files = {}
    components = []
    for image in images:
        base = image.name[: -len('.raw')]
        catalog_id = (catalog_ids or {}).get(image.component, image.component)
        files[f'{base}.manifold.json'] = json.dumps(image.manifest(), indent=3).encode()
        extra = ''
        if image.kind == 'app' and not image.sdk:
            files[f'catalog/icons/64x64/{image.component}.jxl'] = JXL_DATA
            files[f'catalog/media/{image.component}/screenshot.jxl'] = JXL_DATA
            extra = (
                f'<icon type="cached" width="64" height="64">{image.component}.jxl</icon>'
                '<screenshots><screenshot type="default">'
                f'<image type="source">{image.component}/screenshot.jxl</image>'
                '</screenshot></screenshots>'
            )
        ref = f'{image.architecture}/{image.component}/{image.version}'
        components.append(
            f'<component type="generic"><id>{catalog_id}</id><name>{catalog_id}</name>{extra}'
            f'<custom><value key="manifold.id">{ref}</value></custom></component>\n'
        )
    files['catalog/manifold-local.xml'] = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<components version="1.0" origin="manifold-local">\n'
        + ''.join(components)
        + '</components>\n'
    ).encode()
    index = {
        'format': 1,
        'files': [
            {
                'name': name,
                'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
                'blake3': blake3_hex(data),
            }
            for name, data in sorted(files.items())
        ],
    }
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED) as bundle:
        for image in images:
            data = image.data
            if image is corrupt_image:
                data = bytes([data[0] ^ 0xFF]) + data[1:]
            bundle.writestr(image.name, data)
        for name, data in files.items():
            bundle.writestr(name, data)
        bundle.writestr('index.json', json.dumps(index, indent=3))
        if signed:
            bundle.writestr('index.json.sig', 'not a signature\n')
    return path
