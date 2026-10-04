#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
What the end-to-end tests share: how a daemon is started and stopped, how
it is called, and how bundles are made up. Calls are made with varlinkctl.

The tests run in the sandbox, which `mf-test-sandbox` enters for them: the
directories Manifold keeps its state in are empty, and we are root.
"""

import hashlib
import json
import os
import platform
import re
import signal
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


class VarlinkError(Exception):
    """A call was replied to with an error. varlinkctl turns the error into a
    message, and prints the parameters of the error, which are kept here."""

    def __init__(self, message, parameters):
        super().__init__(message)
        self.parameters = parameters


class Daemon:
    """A daemon of Manifold, listening on a socket of our choosing."""

    def __init__(self, exe, socket_path):
        self.exe = exe
        self.socket_path = socket_path
        self.process = None
        self.log = None

    def start(self):
        self.log = None
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        self.process = subprocess.Popen(
            [self.exe, '--socket', self.socket_path, '--verbose'],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        for _ in range(200):
            if os.path.exists(self.socket_path):
                return
            if self.process.poll() is not None:
                raise RuntimeError(f'{self.exe} exited early:\n{self.process.stderr.read()}')
            time.sleep(0.05)
        raise RuntimeError(f'{self.exe} did not start listening')

    def stop(self):
        """Stop the daemon, and return what it logged. Stopping it twice is fine."""
        if self.log is not None:
            return self.log
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        _, self.log = self.process.communicate(timeout=10)
        if self.process.returncode != 0:
            raise RuntimeError(
                f'{self.exe} exited with status {self.process.returncode}:\n{self.log}'
            )
        return self.log

    def call(self, method, parameters=None, more=False, fd=None):
        """
        Call a Varlink method, and return the parameters of the reply. With
        `more`, every reply is returned, as a list. A file descriptor is passed
        along as the first one, index 0. An error reply raises VarlinkError.
        """
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


def write_bundle(path, images, signed=True, corrupt_image=None, metainfo_ids=None):
    """Write a .manifold file as the builder would. metainfo_ids maps a component
    to the identifier its MetaInfo claims, when that is to differ."""
    files = {}
    for image in images:
        base = image.name[: -len('.raw')]
        metainfo_id = (metainfo_ids or {}).get(image.component, image.component)
        files[f'{base}.manifold.json'] = json.dumps(image.manifest(), indent=3).encode()
        files[f'{base}.metainfo.xml'] = (
            f'<component><id>{metainfo_id}</id><name>{metainfo_id}</name></component>\n'.encode()
        )
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
