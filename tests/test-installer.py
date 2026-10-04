#!/usr/bin/env python3
#
# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
End-to-end tests of manifoldd and the manifold tool, in the sandbox: what
a user sees when installing and removing bundles. The store itself is
tested in test-store.c.

Usage: mf-test-sandbox test-installer.py --daemon manifoldd --client manifold
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mftest import (  # noqa: E402
    APP_ID,
    CORE_ID,
    RUNTIME_DIR,
    STORE,
    Daemon,
    Image,
    VarlinkError,
    host_architecture,
    write_bundle,
)

SOCKET = f'{RUNTIME_DIR}/test-manifoldd.sock'

ARGS = None


class InstallerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='manifold-test-')
        cls.daemon = Daemon(ARGS.daemon, SOCKET)
        cls.daemon.start()

        cls.core = Image(CORE_ID, 'core', '13.4')
        cls.sdk = Image(CORE_ID + '.sdk', 'core', '13.4', sdk=True, base=cls.core)
        cls.kit = Image(CORE_ID + '.7d225c1fe79b1c5e', 'kit', '20260114T0238', base=cls.core)
        cls.app = Image(APP_ID, 'app', '0.1.0', base=cls.kit)
        cls.app2 = Image(APP_ID, 'app', '0.2.0', base=cls.kit)

        cls.core_bundle = cls.bundle('core', [cls.core, cls.sdk])
        cls.app_bundle = cls.bundle('app', [cls.kit, cls.app])
        cls.app2_bundle = cls.bundle('app2', [cls.kit, cls.app2])
        cls.kit_bundle = cls.bundle('kit', [cls.kit])

    @classmethod
    def tearDownClass(cls):
        cls.daemon.stop()
        shutil.rmtree(cls.work, ignore_errors=True)

    @classmethod
    def bundle(cls, name, images, **kwargs):
        return write_bundle(os.path.join(cls.work, name + '.manifold'), images, **kwargs)

    def setUp(self):
        """Every test starts with nothing installed."""
        for name in os.listdir(STORE):
            shutil.rmtree(os.path.join(STORE, name))
        for name in ('images/by-verity', 'apps', 'plugins', 'runtimes'):
            os.makedirs(os.path.join(STORE, name))

    def manifold(self, *args, expect=0):
        """Run the manifold tool, check its exit status, and return its output."""
        result = subprocess.run(
            [ARGS.client, '--socket', SOCKET, *args], capture_output=True, text=True, check=False
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, expect, f'manifold {" ".join(args)}:\n{output}')
        return output

    def installed(self):
        """The identifiers of the installed components, with their version and mode,
        as the store on disk records them: there is no call for that."""
        found = []
        for subdir in ('apps', 'plugins', 'runtimes'):
            for component_id in os.listdir(f'{STORE}/{subdir}'):
                for name in os.listdir(f'{STORE}/{subdir}/{component_id}'):
                    version_dir = f'{STORE}/{subdir}/{component_id}/{name}'
                    if name.startswith('.') or os.path.islink(version_dir):
                        continue
                    mode = 'manual'
                    if os.path.exists(f'{version_dir}/mode'):
                        with open(f'{version_dir}/mode', encoding='utf-8') as f:
                            mode = f.read().strip()
                    version = name.rsplit('-', 1)[0]
                    found.append(
                        (component_id, version, 'dependency' if mode == 'auto' else 'manual')
                    )
        return sorted(found)

    def stored_images(self):
        return sorted(name[: -len('.raw')] for name in os.listdir(f'{STORE}/images/by-verity'))

    # the tool

    def test_usage(self):
        self.assertIn('Manifold version', self.manifold('--version'))
        # 2 is a wrong invocation, 3 a bad input
        self.manifold(expect=2)
        self.assertIn('unknown', self.manifold('frobnicate', expect=2))
        self.manifold('install', expect=3)
        self.manifold('list', '--kind', 'bogus', expect=2)
        self.manifold('cleanup', 'extra', expect=2)
        self.manifold('install', '/nonexistent.manifold', expect=3)

    def test_not_a_bundle(self):
        notes = os.path.join(self.work, 'notes.txt')
        with open(notes, 'w', encoding='utf-8') as f:
            f.write('hello\n')
        self.assertIn('Not a valid bundle', self.manifold('install', notes, expect=1))

    # installing

    def test_install_core(self):
        out = self.manifold('install', self.core_bundle)
        self.assertIn(f'core     {CORE_ID} ', out)
        self.assertIn('Disk space used', out)
        self.assertEqual(
            self.installed(), [(CORE_ID, '13.4', 'manual'), (CORE_ID + '.sdk', '13.4', 'manual')]
        )
        self.assertEqual(
            self.stored_images(), sorted([self.core.verity_hash, self.sdk.verity_hash])
        )

        out = self.manifold('list')
        self.assertIn(f'core     {CORE_ID} ', out)
        self.assertIn('sdk current', out)
        self.assertIn('Nothing is installed', self.manifold('list', '--kind', 'app'))

    def test_app_needs_its_core(self):
        self.assertIn(
            f'needs base {CORE_ID} 13.4', self.manifold('install', self.app_bundle, expect=1)
        )
        self.assertEqual(self.stored_images(), [])

    def test_app_brings_its_kit(self):
        self.manifold('install', self.core_bundle)
        self.assertIn(f'app      {APP_ID} ', self.manifold('install', self.app_bundle))
        self.assertIn((self.kit.component, self.kit.version, 'dependency'), self.installed())
        self.assertIn((APP_ID, '0.1.0', 'manual'), self.installed())
        self.assertIn('dependency', self.manifold('list', '--kind', 'kit'))

    def test_wrong_architecture_is_refused(self):
        other = 'aarch64' if host_architecture() == 'x86_64' else 'x86_64'
        core = Image(CORE_ID, 'core', '13.4', architecture=other)
        bundle = self.bundle('foreign', [core])

        self.assertIn(
            f'is built for {other}, but this system is {host_architecture()}',
            self.manifold('install', bundle, expect=1),
        )
        self.assertEqual(self.installed(), [])
        self.assertEqual(self.stored_images(), [])

    def test_install_again_is_refused(self):
        self.manifold('install', self.core_bundle)
        self.assertIn('installed already', self.manifold('install', self.core_bundle, expect=1))
        self.assertEqual(len(self.stored_images()), 2)

    def test_same_version_with_other_image_is_refused(self):
        other_kit = Image(
            self.kit.component,
            'kit',
            self.kit.version,
            base=self.core,
            seed='other',
        )
        other_bundle = self.bundle('conflict', [other_kit, self.app])
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)

        self.assertIn('installed already', self.manifold('install', other_bundle, expect=1))
        self.assertNotIn(other_kit.verity_hash, self.stored_images())
        self.assertNotIn((APP_ID, '0.1.0', 'manual'), self.installed())

    def test_base_is_what_provides_the_identity(self):
        # a kit with another ID, but the same identity as the kit the app needs
        renamed_kit = Image(CORE_ID + '.renamed', 'kit', '20260118T0000', base=self.core)
        renamed_kit.identity = self.kit.identity
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.bundle('renamed-kit', [renamed_kit]))

        self.manifold('install', self.bundle('app-only', [self.app]))
        self.assertEqual(
            os.readlink(f'{STORE}/apps/{APP_ID}/0.1.0-{self.app.revision}/base'),
            f'../../../runtimes/{renamed_kit.component}/current',
        )

    def test_platform_sits_on_its_exact_core(self):
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', revision=22910119, base=self.core)
        self.manifold('install', self.core_bundle)
        self.assertIn('platform', self.manifold('install', self.bundle('platform', [platform])))
        self.assertEqual(
            os.readlink(f'{STORE}/runtimes/{platform.component}/6.8-22910119/base'),
            f'../../../runtimes/{CORE_ID}/13.4-{self.core.revision}',
        )
        self.assertIn(f'{platform.component} 6.8-22910119', self.manifold('list'))

    def test_platform_must_sit_on_a_core(self):
        platform = Image(CORE_ID + '.qt6', 'platform', '6.8', base=self.kit)
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.kit_bundle)
        self.assertIn(
            'the base of a platform must be a core, not the kit',
            self.manifold('install', self.bundle('platform-on-kit', [platform]), expect=1),
        )
        self.assertNotIn(platform.verity_hash, self.stored_images())

    def test_corrupt_bundle_leaves_nothing(self):
        corrupt = self.bundle('corrupt', [self.kit, self.app], corrupt_image=self.app)
        self.manifold('install', self.core_bundle)
        self.assertIn('could not be verified', self.manifold('install', corrupt, expect=1))
        self.assertNotIn(self.app.verity_hash, self.stored_images())
        for root, dirs, files in os.walk(STORE):
            for name in dirs + files:
                self.assertFalse(name.startswith('.tmp-'), f'leftover {root}/{name}')

    def test_install_reports_progress(self):
        with open(self.core_bundle, 'rb') as f:
            replies = self.daemon.call(
                'org.freedesktop.manifold.InstallFile',
                {'bundleFileDescriptor': 0},
                more=True,
                fd=f.fileno(),
            )
        items = [r['progress']['item'] for r in replies[:-1]]
        self.assertEqual([i['action'] for i in items], ['install'] * 4)
        self.assertEqual([i['percent'] for i in items], [0, 100, 0, 100])
        self.assertEqual([r['progress']['percent'] for r in replies[:-1]], [0, 50, 50, 100])
        self.assertEqual(items[0]['bytesTotal'], len(self.core.data))
        result = replies[-1]['result']
        self.assertEqual(result['bytesAdded'], len(self.core.data) + len(self.sdk.data))
        self.assertEqual(
            {c['componentId'] for c in result['installed']}, {CORE_ID, CORE_ID + '.sdk'}
        )

    def test_metainfo_is_kept_and_linked(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        farm = f'{STORE}/origins/local/metainfo'
        for directory, component in (('runtimes', CORE_ID), ('apps', APP_ID)):
            name = f'{component}.metainfo.xml'
            self.assertTrue(os.path.isfile(f'{STORE}/{directory}/{component}/current/{name}'))
            self.assertEqual(
                os.readlink(f'{farm}/{name}'), f'../../../{directory}/{component}/current/{name}'
            )
            with open(f'{farm}/{name}', encoding='utf-8') as f:
                self.assertIn(f'<id>{component}</id>', f.read())
        self.manifold('remove', APP_ID)
        self.assertFalse(os.path.lexists(f'{farm}/{APP_ID}.metainfo.xml'))
        self.assertTrue(os.path.lexists(f'{farm}/{CORE_ID}.metainfo.xml'))

    def test_metainfo_must_describe_the_component(self):
        bundle = self.bundle(
            'wrong-metainfo', [self.core, self.sdk], metainfo_ids={CORE_ID: 'org.example.Other'}
        )
        out = self.manifold('install', bundle, expect=1)
        self.assertIn('describes org.example.Other, not', out)
        self.assertEqual(self.installed(), [])

    # removing

    def test_remove_takes_the_kit_along(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        out = self.manifold('remove', APP_ID)
        self.assertIn(f'app      {APP_ID} ', out)
        self.assertIn(f'kit      {self.kit.component} ', out)
        self.assertIn('Removed 2 images', out)
        self.assertEqual(
            self.installed(), [(CORE_ID, '13.4', 'manual'), (CORE_ID + '.sdk', '13.4', 'manual')]
        )

    def test_kit_installed_on_purpose_stays(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.manifold('install', self.kit_bundle)
        self.assertIn((self.kit.component, self.kit.version, 'manual'), self.installed())
        self.manifold('remove', APP_ID)
        self.assertIn((self.kit.component, self.kit.version, 'manual'), self.installed())

    def test_what_is_in_use_stays(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.assertIn('is used by', self.manifold('remove', self.kit.component, expect=1))
        self.assertIn('is used by', self.manifold('remove', CORE_ID, expect=1))
        self.assertIn('not installed', self.manifold('remove', 'org.example.nothing', expect=1))
        self.assertIn('not installed', self.manifold('remove', APP_ID, '9.9.9', expect=1))

    def test_several_versions(self):
        self.manifold('install', self.core_bundle)
        self.manifold('install', self.app_bundle)
        self.manifold('install', self.app2_bundle)
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 2)

        # without a terminal, a removal of one of several versions has to name it
        self.assertIn('Several versions', self.manifold('remove', APP_ID, expect=1))
        self.manifold('remove', APP_ID, '0.2.0')
        self.assertEqual(self.manifold('list', APP_ID).count(APP_ID), 1)
        self.assertIn((APP_ID, '0.1.0', 'manual'), self.installed())

    def test_cleanup(self):
        self.manifold('install', self.core_bundle)
        self.assertIn('Nothing to clean up', self.manifold('cleanup'))

        stray = f'{STORE}/images/by-verity/{"f" * 64}.raw'
        with open(stray, 'wb') as f:
            f.write(b'x' * 4096)
        os.mkdir(f'{STORE}/runtimes/{CORE_ID}/.tmp-9.9.9-1')
        out = self.manifold('cleanup')
        self.assertIn('Removed 1 image', out)
        self.assertFalse(os.path.exists(stray))
        self.assertFalse(os.path.exists(f'{STORE}/runtimes/{CORE_ID}/.tmp-9.9.9-1'))

        with open(stray, 'wb') as f:
            f.write(b'x' * 4096)
        replies = self.daemon.call('org.freedesktop.manifold.Cleanup', more=True)
        self.assertEqual([r['progress']['percent'] for r in replies[:-1]], [0, 100])
        self.assertEqual(replies[-1]['result']['removedImages'], ['f' * 64])
        self.assertEqual(replies[-1]['result']['bytesFreed'], 4096)

    def test_transaction_flags(self):
        self.daemon.call('org.freedesktop.manifold.Cleanup', {'flags': ['allowInteractive']})
        # an unknown flag is an invalid parameter, and the error names the field
        with self.assertRaises(VarlinkError) as refused:
            self.daemon.call('org.freedesktop.manifold.Cleanup', {'flags': ['bogus']})
        self.assertEqual(refused.exception.parameters, {'parameter': 'flags'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--daemon', required=True)
    parser.add_argument('--client', required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
