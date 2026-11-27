#!/usr/bin/env python3
"""Synthetic contract/negative tests; never invoke SDK installers, Gradle or Docker."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('doctor', Path(__file__).with_name('doctor.py'))
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


def openssl_recipe(repo):
    return hashlib.sha256((repo / 'android/openssl/build.sh').read_bytes()
                          + (repo / 'android/tools/ndk_toolchain.sh').read_bytes()).hexdigest()


class DoctorTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='android-doctor-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / '中文 空格'
        self.root.mkdir()
        self.repo = self.root / 'repo'
        pins = d.requirements(d.REPO)
        for name in [*pins['sources'], 'services/opus/s2a_opus.c', 'services/vpx/s2a_vp8.c', 'services/vpx/s2a_vp9.c',
                     'services/vpx/patches/0001-s2a-vp9-nvdec-hook.patch']:
            dest = self.repo / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(d.REPO / name, dest)
        self.pins = pins
        self.args = argparse.Namespace(sdk=str(self.root / 'sdk'), cache_root=str(self.root / 'cache'),
            gradle_cache=None, nuget_cache=None, jdk=str(self.root / 'jdk'), gradle_home=str(self.root / 'gradle'),
            dotnet=str(self.root / 'dotnet'), assets=None, docker=str(self.root / 'docker'),
            image=None, offline=False, config=None, space_path=str(self.root), min_free_bytes=0, format='json', plan=False)

    def write(self, path, data=b'x'):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def elf(self, path, machine=183):
        h = bytearray(64)
        h[:6] = b'\x7fELF\x02\x01'
        h[16:18] = struct.pack('<H', 3)
        h[18:20] = struct.pack('<H', machine)
        self.write(path, h).chmod(0o700)

    def archive(self, path, machine=183):
        obj = bytearray(64)
        obj[:6] = b'\x7fELF\x02\x01'
        obj[16:18] = struct.pack('<H', 1)
        obj[18:20] = struct.pack('<H', machine)
        header = b'object.o/       ' + b'0           ' + b'0     ' + b'0     ' + b'100644  ' + str(len(obj)).encode().ljust(10) + b'`\n'
        self.write(path, b'!<arch>\n' + header + obj)

    def jar(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(path,'w') as z:
            z.writestr('example/Entry.class', b'fixture bytecode')


    def complete(self):
        # Synthetic payloads test inventory shape only; offline/build remains unverified.
        p = self.pins
        java = self.write(Path(self.args.jdk) / 'bin/javac')
        java.chmod(0o700)
        runtime=self.write(Path(self.args.jdk) / 'bin/java'); runtime.chmod(0o700)
        sdk = Path(self.args.sdk)
        ndk_prebuilt = 'toolchains/llvm/prebuilt/linux-x86_64/bin'
        for rel, pkg, files in [
            ('platforms/android-' + p['sdk'], 'platforms;android-' + p['sdk'], ['android.jar']),
            ('build-tools/' + p['build_tools'], 'build-tools;' + p['build_tools'], ['aapt2','apksigner','zipalign']),
            ('ndk/' + p['ndk'], 'ndk;' + p['ndk'], ['build/cmake/android.toolchain.cmake', ndk_prebuilt + '/clang', ndk_prebuilt + '/aarch64-linux-android' + p['api'] + '-clang']),
            ('cmake/' + p['cmake'], 'cmake;' + p['cmake'], ['bin/cmake','bin/ninja'])]:
            revision = ''
            if pkg.split(';')[0] != 'platforms':
                major, minor, micro = pkg.split(';')[1].split('.')
                revision = f'<revision><major>{major}</major><minor>{minor}</minor><micro>{micro}</micro></revision>'
            self.write(sdk / rel / 'package.xml', f'<root><localPackage path="{pkg}">{revision}</localPackage></root>'.encode())
            for f in files:
                if f.endswith('.jar'): self.jar(sdk / rel / f)
                elif f.endswith('.cmake'): self.write(sdk / rel / f)
                elif f == 'apksigner' or f.endswith('-clang'):
                    self.write(sdk / rel / f, b'#!/bin/sh\nexit 0\n').chmod(0o700)
                else: self.elf(sdk / rel / f, 62)
        gradle = Path(self.args.gradle_home)
        self.jar(gradle / 'lib' / ('gradle-launcher-' + p['gradle'] + '.jar'))
        self.write(gradle / 'bin/gradle', b'#!/bin/sh\nexit 0\n').chmod(0o700)
        cache = Path(self.args.cache_root)
        for ext in ('pom', 'jar'):
            dest = cache / 'gradle/caches/modules-2/files-2.1/com.android.tools.build/gradle' / p['agp'] / 'hash' / ('gradle.' + ext)
            if ext == 'jar': self.jar(dest)
            else: self.write(dest, f'<project><artifactId>gradle</artifactId><version>{p["agp"]}</version></project>'.encode())
        self.write(cache / 'dotnet' / p['cs_sdk'] / 'sdk' / p['cs_sdk'] / 'dotnet.dll')
        self.elf(cache / 'dotnet' / p['cs_sdk'] / 'dotnet', 62)
        runtime = '10.0.7'
        sdk_rid = p['sdk_hosts']['Linux-x86_64']   # 测试把宿主 mock 成 Linux x64
        self.write(self.repo / 'build/android-cs/probe/obj/project.assets.json', json.dumps({
            'project': {'frameworks': {'net10.0': {'downloadDependencies': [
                {'name': 'Microsoft.NETCore.App.Runtime.NativeAOT.' + p['target'], 'version': '[' + runtime + ',' + runtime + ']'}]}}},
            'libraries': {'runtime.' + sdk_rid + '.Microsoft.DotNet.ILCompiler/10.0.8': {}}
        }).encode())
        rt = cache / 'nuget' / ('microsoft.netcore.app.runtime.nativeaot.' + p['target']) / runtime
        self.archive(rt / ('runtimes/' + p['target'] + '/native/libRuntime.WorkstationGC.a'))
        for f in ('LICENSE.TXT','THIRD-PARTY-NOTICES.TXT'):
            self.write(rt / f)
        self.elf(cache / 'nuget' / ('runtime.' + sdk_rid + '.microsoft.dotnet.ilcompiler/10.0.8/tools/ilc'), 62)
        ssl = cache / p['openssl_cache']
        for f in ('libssl.so','libcrypto.so'):
            self.elf(ssl / f)
        self.write(ssl / 'LICENSE.txt')
        for name, archive, shim, notices, header in [
            ('opus','libopus.a','s2a_opus',['COPYING'],'include/opus.h'),
            ('vpx','libvpx.a','s2a_vpx',['LICENSE','PATENTS'],'include/vpx/vpx_decoder.h')]:
            # codec 目录实名为 <name>-<ver>-<64 位 recipe key>；key 折进工具链指纹，doctor 只认形状。
            root = cache / 'codecs' / (name + '-' + p[name]['version'] + '-' + 'ab' * 32)
            self.archive(root / archive)
            for f in [header, *notices]:
                self.write(root / f)
            self.manifest(root, [archive, header, *notices])
            shimdir = root / ('s2a-' + 'cd' * 32)
            self.archive(shimdir / ('lib' + shim + '.a'))
            self.manifest(shimdir, ['lib' + shim + '.a'])

    def manifest(self, root, names):
        lines = [hashlib.sha256((root / n).read_bytes()).hexdigest() + '  ./' + n for n in names]
        self.write(root / 'manifest.sha256', ('\n'.join(lines) + '\n').encode())

    def codec_dir(self, name):
        hits = sorted((Path(self.args.cache_root) / 'codecs').glob(
            name + '-' + self.pins[name]['version'] + '-*'))
        self.assertEqual(len(hits), 1)
        return hits[0]

    def runner(self, argv):
        # An unexpected command means this test attempted a mutating operation.
        if argv[-1] == '-version':
            if Path(argv[0]).name == 'java': return 0, 'openjdk version \"' + self.pins['jdk'] + '.0.1\"'
            return 0, 'javac ' + self.pins['jdk'] + '.0.1'
        if argv[-1] == '--list-sdks': return 0, self.pins['cs_sdk'] + ' [/cache/sdk]\n'
        if argv[1:3] == ['version', '--format']:
            return 0, json.dumps({'Server': {'Version':'test','Os':'linux','Arch':'amd64'}})
        if argv[1:3] == ['image','inspect']:
            return 0, json.dumps({'Id':'sha256:test','Os':'linux','Architecture':'amd64'})
        self.fail('unexpected command: ' + repr(argv))

    def inspect(self, runner=None):
        with patch.object(d, 'run', runner or self.runner), patch.object(d.platform, 'system', return_value='Linux'), patch.object(d.platform, 'machine', return_value='x86_64'):
            return d.inspect(self.args, self.repo)

    def check(self, name, report=None):
        return next(c for c in (report or self.inspect())['checks'] if c['id'] == name)

    def test_source_pins_and_cache_expressions(self):
        self.assertEqual(self.pins['gradle'], '9.8.1')
        self.assertEqual(self.pins['ndk'], '29.0.14206865')
        self.assertEqual(self.pins['openssl_cache'],
                         'openssl/3.5.9-ndk29.0.14206865-api34-' + openssl_recipe(self.repo))
        self.assertIn('android/tools/jdk17.sh', self.pins['sources'])

    def test_source_drift_fails(self):
        p = self.repo / 'android/app/build.gradle'
        p.write_text(p.read_text().replace("ndkVersion '29.0.14206865'", "ndkVersion '30.0.1'"))
        with self.assertRaises(ValueError): d.requirements(self.repo)

    def test_ambiguous_pin_fails(self):
        p = self.repo / 'android/docker/Dockerfile'
        p.write_text(p.read_text() + '\nARG GRADLE=10.0.0\n')
        with self.assertRaises(ValueError): d.requirements(self.repo)

    def test_fully_present_inventory_not_build_proof(self):
        self.complete()
        r = self.inspect()
        self.assertEqual(r['status'], 'incomplete')
        self.assertEqual({c['id'] for c in r['checks'] if c['status'] != 'ok'}, {'offline_resolution'})
        for k,v in r['stages'].items():
            self.assertEqual(v, 'incomplete' if k == 'environment' else 'not_tested')

    def test_missing_payload_in_named_package(self):
        self.complete()
        (Path(self.args.sdk) / 'platforms/android-36/android.jar').unlink()
        self.assertEqual(self.check('sdk_files')['status'], 'missing')

    def test_mismatched_package_metadata(self):
        self.complete()
        p = Path(self.args.sdk) / 'ndk' / self.pins['ndk'] / 'package.xml'
        p.write_text('<root><localPackage path="ndk;wrong"/></root>')
        self.assertEqual(self.check('ndk_package')['status'], 'mismatch')

    def test_package_revision_mismatch(self):
        self.complete()
        p = Path(self.args.sdk) / 'cmake' / self.pins['cmake'] / 'package.xml'
        p.write_text(p.read_text().replace('<major>3</major>','<major>2</major>'))
        self.assertEqual(self.check('cmake_package')['status'],'mismatch')

    def test_wrong_gradle_metadata(self):
        self.complete()
        p = Path(self.args.gradle_home) / 'lib'
        next(p.glob('*.jar')).rename(p / 'gradle-launcher-9.7.1.jar')
        self.assertEqual(self.check('gradle')['status'], 'mismatch')

    def test_missing_agp_jar(self):
        self.complete()
        for p in (Path(self.args.cache_root) / 'gradle').rglob('*.jar'): p.unlink()
        self.assertEqual(self.check('agp_jar')['status'], 'missing')

    def test_wrong_jdk_and_dotnet(self):
        self.complete()
        def runner(argv):
            if argv[-1] == '-version': return 0, 'javac 21.0.1'
            if argv[-1] == '--list-sdks': return 0, '6.0.100 [/cache/sdk]'
            return self.runner(argv)
        r = self.inspect(runner)
        self.assertEqual(self.check('jdk', r)['status'], 'mismatch')
        self.assertEqual(self.check('dotnet_inventory', r)['status'], 'mismatch')

    def test_corrupt_openssl_wrong_arch(self):
        self.complete()
        self.elf(Path(self.args.cache_root) / self.pins['openssl_cache'] / 'libssl.so', 62)
        self.assertEqual(self.check('openssl')['status'], 'missing')

    def test_license_required(self):
        self.complete()
        (self.codec_dir('vpx') / 'PATENTS').unlink()
        self.assertEqual(self.check('vpx')['status'], 'missing')

    def test_corrupt_archive(self):
        self.complete()
        (self.codec_dir('opus') / 'libopus.a').write_bytes(b'not ar')
        self.assertEqual(self.check('opus')['status'], 'missing')

    def test_shim_missing_or_corrupt(self):
        self.complete()
        root = self.codec_dir('opus')
        for shim in root.glob('s2a-*'):
            shutil.rmtree(shim)
        self.assertEqual(self.check('opus')['status'], 'missing')

    def test_runtime_never_guessed(self):
        self.complete()
        p = self.repo / 'build/android-cs/probe/obj/project.assets.json'
        p.write_text('{}')
        self.assertEqual(self.check('nativeaot_runtime')['status'], 'unverified')

    def test_missing_runtime_archive(self):
        self.complete()
        for p in (Path(self.args.cache_root) / 'nuget').rglob('*.a'): p.unlink()
        self.assertEqual(self.check('nativeaot_runtime')['status'], 'missing')

    def test_wrong_cached_sdk_architecture(self):
        self.complete()
        self.elf(Path(self.args.cache_root) / 'dotnet' / self.pins['cs_sdk'] / 'dotnet', 183)
        self.assertEqual(self.check('nativeaot_sdk_host')['status'], 'mismatch')

    def test_compiler_exact_restored_version(self):
        self.complete()
        self.assertEqual(self.check('nativeaot_compiler')['status'], 'ok')
        for p in (Path(self.args.cache_root) / 'nuget').rglob('ilc'): p.unlink()
        self.assertEqual(self.check('nativeaot_compiler')['status'], 'missing')

    def test_docker_unavailable_no_run(self):
        self.complete()
        def runner(argv):
            if argv[0] == self.args.docker: return 1, 'sensitive diagnostics never emitted'
            return self.runner(argv)
        r = self.inspect(runner)
        self.assertEqual(self.check('docker',r)['status'], 'missing')
        self.assertNotIn('sensitive diagnostics', json.dumps(r))

    def test_docker_arm64_is_not_x64(self):
        self.complete()
        def runner(argv):
            if argv[1:3] == ['image','inspect']: return 0, '{"Os":"linux","Architecture":"arm64"}'
            return self.runner(argv)
        self.assertEqual(self.check('docker_image',self.inspect(runner))['status'], 'unsupported')

    def test_host_mac_never_supported_by_image_alone(self):
        # #1131：Darwin arm64 在固定 SDK RID 表内 → 支持；Darwin x86_64 / 其它系统不在表内 → unsupported。
        self.complete()
        for system, machine, want in [('Darwin', 'arm64', 'ok'), ('Darwin', 'x86_64', 'unsupported'),
                                      ('FreeBSD', 'amd64', 'unsupported')]:
            with self.subTest(host=system + '-' + machine), \
                 patch.object(d, 'run', self.runner), \
                 patch.object(d.platform, 'system', return_value=system), \
                 patch.object(d.platform, 'machine', return_value=machine):
                r = d.inspect(self.args, self.repo)
            self.assertEqual(self.check('nativeaot_host', r)['status'], want)

    def test_disk_budget_user_defined(self):
        self.complete()
        self.args.min_free_bytes = 2**63
        self.assertEqual(self.check('disk_space')['status'], 'missing')
        self.args.min_free_bytes = None
        self.assertEqual(self.check('disk_space')['status'], 'unverified')

    def test_snapshot_read_only(self):
        self.complete()
        def snapshot(): return {str(p): (p.stat().st_mode, p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).digest()) for p in self.root.rglob('*') if p.is_file()}
        before = snapshot()
        self.inspect()
        self.assertEqual(before,snapshot())

    def test_exact_version_range(self):
        self.assertEqual(d.exact_version('[10.0.7,10.0.7]'),'10.0.7')
        with self.assertRaises(ValueError): d.exact_version('[10.0.7,10.0.8]')

    def test_review_nuget_exact_assets(self):
        self.complete()
        rt = Path(self.args.cache_root) / ('nuget/microsoft.netcore.app.runtime.nativeaot.' + self.pins['target'])
        (rt / '10.0.7').rename(rt / '10.0.14')
        p = self.repo / 'build/android-cs/probe/obj/project.assets.json'
        original = json.loads(p.read_text())
        for version in ('[10.0.14]', '[10.0.14,10.0.14]', '[10.0.14, 10.0.14]'):
            with self.subTest(version=version):
                original['project']['frameworks']['net10.0']['downloadDependencies'][0]['version'] = version
                p.write_text(json.dumps(original))
                self.assertEqual(self.check('nativeaot_restore')['status'], 'ok')
                self.assertEqual(self.check('nativeaot_runtime')['status'], 'ok')
        for version in ('[10.0.14, 10.0.15]', '(10.0.14,10.0.14]', '[10.0.14,)', '10.0.14', '[]', '[,]'):
            with self.subTest(version=version):
                original['project']['frameworks']['net10.0']['downloadDependencies'][0]['version'] = version
                p.write_text(json.dumps(original))
                self.assertEqual(self.check('nativeaot_restore')['status'], 'missing')

    def test_review_ndk_reassignment_all_consumers(self):
        for name, assignment in [('openssl', 'ndk=$ANDROID_HOME/ndk/30.0.1'),
                                 ('codecs', 'ndk=$ANDROID_HOME/ndk/30.0.1'),
                                 ('cs', 'ndk_bin=$ANDROID_HOME/ndk/30.0.1/toolchains/llvm/prebuilt/linux-x86_64/bin')]:
            p = self.repo / ('android/' + name + '/build.sh')
            original = p.read_text()
            for extra in (assignment, assignment.replace('30.0.1', self.pins['ndk']), 'export ' + assignment):
                with self.subTest(name=name, extra=extra):
                    p.write_text(original + '\n' + extra + '\n')
                    with self.assertRaises(ValueError): d.requirements(self.repo)
            p.write_text(original)

    def test_review_ndk_unknown_expression(self):
        p = self.repo / 'android/openssl/build.sh'
        p.write_text(p.read_text().replace(
            'ndk=${ANDROID_HOME:?ANDROID_HOME is required for OpenSSL compilation}/ndk/$S2A_ANDROID_NDK',
            'ndk=$(choose_ndk) # $S2A_ANDROID_NDK'))
        with self.assertRaises(ValueError): d.requirements(self.repo)

    def test_review_ndk_quoted_assignment(self):
        p = self.repo / 'android/openssl/build.sh'
        p.write_text(p.read_text().replace(
            'ndk=${ANDROID_HOME:?ANDROID_HOME is required for OpenSSL compilation}/ndk/$S2A_ANDROID_NDK',
            'ndk="${ANDROID_HOME:?ANDROID_HOME is required for OpenSSL compilation}/ndk/$S2A_ANDROID_NDK"'))
        self.assertEqual(d.requirements(self.repo)['ndk'], self.pins['ndk'])

    def test_review_ndk_direct_drift_and_bad_quotes(self):
        for name in ('openssl', 'codecs', 'cs'):
            p = self.repo / ('android/' + name + '/build.sh')
            original = p.read_text()
            p.write_text(original.replace('$S2A_ANDROID_NDK', '30.0.1'))
            with self.subTest(name=name), self.assertRaises(ValueError):
                d.requirements(self.repo)
            p.write_text(original)
        p = self.repo / 'android/openssl/build.sh'
        p.write_text(p.read_text().replace('ndk=${ANDROID_HOME', 'ndk="${ANDROID_HOME'))
        with self.assertRaises(ValueError): d.requirements(self.repo)

    def foreign_compiler_metadata(self, version='[10.0.8, 10.0.8]'):
        # 真实还原的形态（#107 情形 E）：libraries 只有目标侧编译器，宿主编译器只在 downloadDependencies
        p = self.repo / 'build/android-cs/probe/obj/project.assets.json'
        doc = json.loads(p.read_text())
        doc['libraries'] = {'runtime.linux-arm64.Microsoft.DotNet.ILCompiler/10.0.8': {}}
        doc['project']['frameworks']['net10.0']['downloadDependencies'].append(
            {'name': 'runtime.linux-x64.Microsoft.DotNet.ILCompiler', 'version': version})
        p.write_text(json.dumps(doc))
        return Path(self.args.cache_root) / 'nuget/runtime.linux-x64.microsoft.dotnet.ilcompiler/10.0.8'

    def test_review_foreign_compiler_metadata_remains_unverified(self):
        # 负对照（#1568）：宿主编译器只有 downloadDependencies 的钉版本、本地缓存里没有这个包 → 仍 unverified
        self.complete()
        shutil.rmtree(self.foreign_compiler_metadata())
        self.assertEqual(self.check('nativeaot_restore')['status'], 'ok')
        self.assertEqual(self.check('nativeaot_compiler')['status'], 'unverified')

    def test_compiler_pinned_download_with_cached_package(self):
        # 正例（#1568）：精确钉版本 + 缓存里按包名 + 版本找得到 → 再按 x64 ilc 判
        self.complete()
        package = self.foreign_compiler_metadata()
        c = self.check('nativeaot_compiler')
        self.assertEqual((c['status'], c['expected'], c['path']),
                         ('ok', '10.0.8 host executable (downloadDependencies)', str(package / 'tools/ilc')))
        # 只留 .nupkg、没解开：包在，但没有 x64 ilc → missing，不报 ok
        shutil.rmtree(package / 'tools')
        self.write(package / 'runtime.linux-x64.microsoft.dotnet.ilcompiler.10.0.8.nupkg')
        self.assertEqual(self.check('nativeaot_compiler')['status'], 'missing')
        # 包在但 ilc 是别的架构 → mismatch
        self.elf(package / 'tools/ilc', 183)
        self.assertEqual(self.check('nativeaot_compiler')['status'], 'mismatch')

    def test_compiler_download_pin_must_be_exact_and_match_cache(self):
        # 只有其一不算：裸版本是下界、区间不是钉版本、钉的版本缓存里没有 → 都 unverified
        for version in ('10.0.8', '[10.0.8, 10.0.9]', '[10.0.9, 10.0.9]'):
            with self.subTest(version=version):
                self.complete()
                self.foreign_compiler_metadata(version)
                self.assertEqual(self.check('nativeaot_compiler')['status'], 'unverified')

    def gradle_on_path(self, home):
        # PATH 上只放一个指向 <home>/bin/gradle 的软链，同官方构建镜像的 /usr/local/bin/gradle
        bindir = self.root / 'pathbin'
        bindir.mkdir(exist_ok=True)
        link = bindir / 'gradle'
        if link.is_symlink() or link.exists(): link.unlink()
        link.symlink_to(home / 'bin/gradle')
        return patch.dict(os.environ, {'PATH': str(bindir)})

    def test_gradle_found_on_path_like_javac(self):
        # 正例（#1568）：没给 --gradle-home / GRADLE_HOME，PATH 上的 gradle 解软链到 home；不运行 Gradle（self.runner 拦任何意外命令）
        self.complete()
        home = Path(self.args.gradle_home)
        self.args.gradle_home = None
        with self.gradle_on_path(home):
            c = self.check('gradle')
        self.assertEqual((c['status'], c['actual'], c['path']), ('ok', [self.pins['gradle']], str(home)))

    def test_gradle_on_path_negatives(self):
        self.complete()
        home = Path(self.args.gradle_home)
        self.args.gradle_home = None
        # PATH 上没有 gradle → missing（Gradle home 没配）
        with patch.dict(os.environ, {'PATH': str(self.root / 'empty')}):
            self.assertEqual(self.check('gradle')['status'], 'missing')
        # 解出来不在 <home>/bin/ 下（比如包装脚本）→ 不猜 home，missing
        wrapper = self.write(self.root / 'tools/gradle', b'#!/bin/sh\nexit 0\n')
        wrapper.chmod(0o700)
        with self.gradle_on_path(self.root / 'nohome'):
            (self.root / 'pathbin/gradle').unlink()
            (self.root / 'pathbin/gradle').symlink_to(wrapper)
            self.assertEqual(self.check('gradle')['status'], 'missing')
        # PATH 上是别的版本 → mismatch；显式 --gradle-home 仍优先于 PATH
        other = self.root / 'gradle-9.7.1'
        self.jar(other / 'lib/gradle-launcher-9.7.1.jar')
        self.write(other / 'bin/gradle', b'#!/bin/sh\nexit 0\n').chmod(0o700)
        with self.gradle_on_path(other):
            self.assertEqual(self.check('gradle')['status'], 'mismatch')
            self.args.gradle_home = str(home)
            self.assertEqual(self.check('gradle')['status'], 'ok')

    def tool_cases(self):
        sdk = Path(self.args.sdk)
        return [('build_tools_files', sdk / ('build-tools/' + self.pins['build_tools'] + '/aapt2')),
                ('cmake_files', sdk / ('cmake/' + self.pins['cmake'] + '/bin/cmake')),
                ('ndk_compiler', sdk / ('ndk/' + self.pins['ndk'] + '/toolchains/llvm/prebuilt/linux-x86_64/bin/clang')),
                ('nativeaot_sdk', Path(self.args.cache_root) / ('dotnet/' + self.pins['cs_sdk'] + '/dotnet')),
                ('nativeaot_compiler', Path(self.args.cache_root) / ('nuget/runtime.linux-x64.microsoft.dotnet.ilcompiler/10.0.8/tools/ilc'))]

    def test_review_wrong_host_isa(self):
        self.complete()
        for check, p in self.tool_cases():
            with self.subTest(check=check):
                self.elf(p, 183)
                self.assertNotEqual(self.check(check)['status'], 'ok')
                self.elf(p, 62)

    def test_review_tools_need_execute_permission(self):
        self.complete()
        for check, p in self.tool_cases():
            with self.subTest(check=check):
                p.chmod(0o600)
                self.assertNotEqual(self.check(check)['status'], 'ok')
                p.chmod(0o700)

    def test_review_unknown_format_unverified(self):
        self.complete()
        for check, p in self.tool_cases():
            with self.subTest(check=check):
                p.write_bytes(b'unknown executable payload')
                self.assertEqual(self.check(check)['status'], 'unverified')
                self.elf(p, 62)

    def test_review_scripts_and_data_distinguished(self):
        self.complete()
        sdk = Path(self.args.sdk)
        script = sdk / ('build-tools/' + self.pins['build_tools'] + '/apksigner')
        self.assertEqual(self.check('build_tools_files')['status'], 'ok')
        script.chmod(0o600)
        self.assertNotEqual(self.check('build_tools_files')['status'], 'ok')
        script.chmod(0o700)
        script.write_bytes(b'#!/nonexistent/interpreter\nexit 0\n')
        self.assertNotEqual(self.check('build_tools_files')['status'], 'ok')
        self.assertEqual(self.check('ndk_files')['status'], 'ok')
        self.assertEqual(self.check('openssl')['status'], 'ok')

    def test_review_gradle_script_not_nonempty_data(self):
        self.complete()
        p = Path(self.args.gradle_home) / 'bin/gradle'
        p.chmod(0o600)
        self.assertNotEqual(self.check('gradle')['status'], 'ok')
        p.chmod(0o700)
        p.write_bytes(b'not a script')
        self.assertEqual(self.check('gradle')['status'], 'unverified')

    def test_review_macho_host_and_universal(self):
        self.complete()
        sdk = Path(self.args.sdk)
        paths = [sdk / ('build-tools/' + self.pins['build_tools'] + '/' + f) for f in ('aapt2', 'zipalign')]
        arm = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 0, 2, 0, 0, 0, 0)
        x64 = struct.pack('<8I', 0xfeedfacf, 0x01000007, 0, 2, 0, 0, 0, 0)
        def status():
            with patch.object(d, 'run', self.runner), patch.object(d.platform, 'system', return_value='Darwin'), patch.object(d.platform, 'machine', return_value='arm64'):
                return self.check('build_tools_files', d.inspect(self.args, self.repo))['status']
        for p in paths: p.write_bytes(arm)
        self.assertEqual(status(), 'ok')
        paths[0].write_bytes(x64)
        self.assertEqual(status(), 'mismatch')
        paths[0].write_bytes(struct.pack('>7I', 0xcafebabe, 1, 0x0100000c, 0, 28, len(arm), 0) + arm)
        self.assertEqual(status(), 'ok')
        paths[0].write_bytes(paths[0].read_bytes()[:-1])
        self.assertNotEqual(status(), 'ok')

    def test_sdk_rollforward(self):
        self.assertTrue(d.sdk_matches('10.0.404',self.pins['dotnet']))
        self.assertFalse(d.sdk_matches('10.0.402',self.pins['dotnet']))
        self.assertFalse(d.sdk_matches('9.0.301',self.pins['dotnet']))
        self.assertFalse(d.sdk_matches('11.0.301',self.pins['dotnet']))

    def test_cli_json_plan_and_exit(self):
        self.complete()
        with patch.object(d,'REPO',self.repo), patch.object(d,'inspect',return_value=self.inspect()), patch.object(sys,'argv',['doctor','--format','json','--plan','--jdk',"/中文 空格/a'b"]):
            out=io.StringIO()
            with contextlib.redirect_stdout(out): code=d.main()
        r=json.loads(out.getvalue())
        self.assertEqual(code,2)
        self.assertFalse(r['plan']['executed'])
        self.assertEqual(r['plan']['activation']['JAVA_HOME'],"/中文 空格/a'b")

    def test_cli_source_error_nonzero_json(self):
        with patch.object(d,'inspect',side_effect=ValueError('bad source')), patch.object(sys,'argv',['doctor','--format','json']):
            out=io.StringIO()
            with contextlib.redirect_stdout(out): code=d.main()
        self.assertEqual(code,1)
        self.assertEqual(json.loads(out.getvalue())['status'],'error')

    def test_malformed_runtime_metadata_fails_closed(self):
        self.complete()
        p = self.repo / 'build/android-cs/probe/obj/project.assets.json'
        for value in [[], {'project':{'frameworks':[]}}, {'project':{'frameworks':{'net10.0':[]}}},
                      {'project':{'frameworks':{'net10.0':{'downloadDependencies':[None]}}}}]:
            p.write_text(json.dumps(value))
            self.assertEqual(self.check('nativeaot_restore')['status'],'missing')

    def test_malformed_docker_json_fails_closed(self):
        self.complete()
        def runner(argv):
            if argv[0] == self.args.docker: return 0, '[]'
            return self.runner(argv)
        r=self.inspect(runner)
        self.assertEqual(self.check('docker',r)['status'],'error')
        self.assertEqual(self.check('docker_image',r)['status'],'error')

    def test_text_plan_shell_quoting(self):
        self.complete()
        with patch.object(d,'inspect',return_value=self.inspect()), patch.object(sys,'argv',['doctor','--plan','--jdk',"/中文 空格/a'b",'--image','image; echo secret']):
            out=io.StringIO()
            with contextlib.redirect_stdout(out): code=d.main()
        self.assertEqual(code,2)
        import shlex
        activation=next(l for l in out.getvalue().splitlines() if l.startswith('export JAVA_HOME='))
        self.assertEqual(shlex.split(activation)[1], "JAVA_HOME=/中文 空格/a'b")
        command=next(l for l in out.getvalue().splitlines() if l.startswith('docker build'))
        self.assertIn('image; echo secret',shlex.split(command))

    def test_offline_never_contacts_docker(self):
        self.complete()
        self.args.offline = True
        def runner(argv):
            self.assertNotEqual(argv[0], self.args.docker)
            return self.runner(argv)
        r=self.inspect(runner)
        self.assertTrue(r['offline'])
        self.assertEqual(self.check('docker',r)['status'],'unverified')
        self.assertEqual(self.check('docker_image',r)['status'],'unverified')

    def test_corrupt_nonempty_jars(self):
        self.complete()
        for p in (Path(self.args.gradle_home) / 'lib').glob('*.jar'): p.write_bytes(b'nonempty junk')
        self.assertEqual(self.check('gradle')['status'],'missing')
        for p in (Path(self.args.cache_root) / 'gradle').rglob('*.jar'): p.write_bytes(b'PK truncated')
        self.assertEqual(self.check('agp_jar')['status'],'missing')

    def test_corrupt_pom(self):
        self.complete()
        for p in (Path(self.args.cache_root) / 'gradle').rglob('*.pom'): p.write_text('<not-xml')
        self.assertEqual(self.check('agp_pom')['status'],'missing')

    def test_host_archive_rejected(self):
        self.complete()
        self.archive(self.codec_dir('opus') / 'libopus.a',62)
        self.assertEqual(self.check('opus')['status'],'missing')

    def test_archive_truncated_or_empty_rejected(self):
        p=self.root/'lib.a'
        self.archive(p)
        self.assertTrue(d.archive_valid(p,183))
        data=p.read_bytes()
        for value in [b'!<arch>\n',data[:-1],data[:8]+b' '*60,data[:56]+b'9999999999'+data[66:]]:
            p.write_bytes(value)
            self.assertFalse(d.archive_valid(p,183))

    def test_config_strict_types_and_duplicates(self):
        p=self.root/'config.json'
        allowed={'jdk','offline','min_free_bytes'}
        for value in [{'version':True}, {'version':1,'unknown':'x'}, {'version':1,'jdk':''},
                      {'version':1,'offline':'true'}, {'version':1,'min_free_bytes':True},
                      {'version':1,'min_free_bytes':-1}]:
            p.write_text(json.dumps(value))
            with self.assertRaises(ValueError): d.load_config(p,allowed)
        p.write_text('{"version":1,"jdk":"x","jdk":"y"}')
        with self.assertRaises(ValueError): d.load_config(p,allowed)

    def test_cli_config_priority(self):
        p=self.root/'config.json'
        p.write_text(json.dumps({'version':1,'jdk':'/configured java','offline':True}))
        self.complete()
        with patch.object(d,'inspect',return_value=self.inspect()) as inspect, patch.dict(os.environ,{'JAVA_HOME':'/environment'}), patch.object(sys,'argv',['doctor','--format','json','--config',str(p),'--jdk','/explicit java']):
            with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(d.main(),2)
            self.assertEqual(inspect.call_args.args[0].jdk,'/explicit java')
            self.assertTrue(inspect.call_args.args[0].offline)
        with patch.object(d,'inspect',return_value=self.inspect()) as inspect, patch.dict(os.environ,{'JAVA_HOME':'/environment'}), patch.object(sys,'argv',['doctor','--config',str(p)]):
            with contextlib.redirect_stdout(io.StringIO()): d.main()
            self.assertEqual(inspect.call_args.args[0].jdk,'/configured java')

    def test_relative_config_paths_anchor_to_file(self):
        p=self.root/'config.json'
        p.write_text(json.dumps({'version':1,'jdk':'relative java'}))
        config=d.load_config(p,{'jdk'})
        self.assertEqual(config['jdk'],str(self.root/'relative java'))

    def test_bad_config_json_cli_error(self):
        p=self.root/'bad.json';p.write_text('{broken')
        with patch.object(sys,'argv',['doctor','--format','json','--config',str(p)]):
            out=io.StringIO()
            with contextlib.redirect_stdout(out): code=d.main()
        self.assertEqual(code,1)
        self.assertEqual(json.loads(out.getvalue())['status'],'error')

    def test_java_runtime_required_separately(self):
        self.complete()
        (Path(self.args.jdk)/'bin/java').unlink()
        r=self.inspect()
        self.assertEqual(self.check('jdk',r)['status'],'ok')
        self.assertEqual(self.check('jdk_runtime',r)['status'],'missing')

    def test_bounded_failed_probe(self):
        with patch.object(d.subprocess,'run',side_effect=subprocess.TimeoutExpired('docker',8)):
            self.assertEqual(d.run(['docker','version']),(-1,''))


class LiveRequirementsTest(unittest.TestCase):
    def test_requirements_follow_current_host_jdk_source(self):
        pins = d.requirements(d.REPO)
        self.assertEqual(pins['jdk'], '17')
        self.assertIn('android/host/hostenv.sh', pins['sources'])
        self.assertIn('android/tools/jdk17.sh', pins['sources'])
        self.assertNotIn('android/gate.sh', pins['sources'])


class LayoutTest(unittest.TestCase):
    """Exercise the production parser on real sources and bounded synthetic mutations."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='doctor-layout-', dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        # The original parser must also reach each test body for the old-code control.
        for name in ('docker/Dockerfile', 'app/build.gradle', 'settings.gradle', 'cs/build.sh',
                     'cs/nuget-pins.tsv', 'cs/nuget_verify.py',
                     'openssl/build.sh', 'codecs/build.sh', 'codecs/opus-build.sh',
                     'codecs/vpx-build.sh', 'codecs/cache.sh', 'codecs/avc-build.sh',
                     'docker/build.sh', 'docker/gate-apk.sh', 'tools/jdk17.sh',
                     'tools/ndk_toolchain.sh', 'tools/versions.sh', 'tools/compare_apks.py',
                     'tools/compare_backends.sh', 'host/build.sh', 'host/hostenv.sh',
                     'host/prepare.sh', 'host/gate-apk.sh'):
            dest = self.repo / 'android' / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(d.REPO / 'android' / name, dest)
        for name in ('compat/tree/global.json', 'services/opus/build.sh', 'services/vpx/build.sh',
                     'pack/targets/android/package.py'):
            dest = self.repo / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(d.REPO / name, dest)

    def change(self, name, before, after):
        p = self.repo / 'android' / name
        text = p.read_text()
        self.assertIn(before, text)
        p.write_text(text.replace(before, after))

    def roots(self, cs=False, ssl=False):
        if cs:
            self.change('cs/build.sh', 'cache=${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}\n', '')
            self.change('cs/build.sh', '$cache/dotnet/$sdk_version', '/cache/dotnet/$sdk_version')
        if ssl:
            self.change('openssl/build.sh', 'cache_root=${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}\n', '')
            self.change('openssl/build.sh', '$cache_root/', '/cache/')

    def assert_pins(self):
        try:
            p = d.requirements(self.repo)
        except (ValueError, OSError) as exc:
            self.fail('production requirements rejected supported sources: ' + str(exc))
        sdk = json.loads((self.repo / 'compat/tree/global.json').read_text())['sdk']['version']
        self.assertEqual(p['cs_sdk'], sdk)
        self.assertEqual(p['jdk'], '17')
        self.assertEqual(p['openssl_cache'], 'openssl/' + p['openssl'] + '-ndk' + p['ndk'] + '-api' + p['api']
                         + '-' + openssl_recipe(self.repo))
        for name in ('opus', 'vpx'):
            self.assertEqual(p[name + '_cache'], 'codecs/' + name + '-' + p[name]['version'] + '-')
        return p

    def test_combined_real_sources(self):
        pins = self.assert_pins()
        helper = 'android/tools/jdk17.sh'
        self.assertEqual(pins['sources'][helper], hashlib.sha256((self.repo / helper).read_bytes()).hexdigest())

    def test_old_and_mixed_roots(self):
        import itertools
        for flags in itertools.product((False, True), repeat=2):
            with self.subTest(old_cs_ssl=flags):
                # Independent mutations: restore the real producer text each time.
                for name in ('cs/build.sh', 'openssl/build.sh'):
                    shutil.copyfile(d.REPO / 'android' / name,
                                    self.repo / 'android' / name)
                self.roots(*flags)
                self.assert_pins()

    def test_braced_and_quoted_cache_references(self):
        self.change('cs/build.sh', 'DOTNET_ROOT=$cache/dotnet/$sdk_version',
                    'DOTNET_ROOT="${cache}/dotnet/${sdk_version}"')
        self.change('openssl/build.sh',
                    'cache=$cache_root/openssl/$version-ndk$S2A_ANDROID_NDK-api$api-$recipe_sha',
                    'cache="${cache_root}/openssl/${version}-ndk${S2A_ANDROID_NDK}-api${api}-${recipe_sha}"')
        self.assert_pins()

    def test_codec_recipe_braced_root_rejected(self):
        self.change('codecs/opus-build.sh', 'target=$cache_root/codecs/opus-$opus_ver-$key',
                    'target=${cache_root}/codecs/opus-$opus_ver-$key')
        with self.assertRaises(ValueError): d.requirements(self.repo)

    def test_independent_version_pins_remain_source_driven(self):
        self.change('cs/build.sh', 'sdk_version=10.0.403', 'sdk_version=10.0.499')
        p = self.repo / 'compat/tree/global.json'
        doc = json.loads(p.read_text()); doc['sdk']['version'] = '10.0.499'
        p.write_text(json.dumps(doc))
        self.change('openssl/build.sh', 'version=3.5.9', 'version=3.5.10')
        self.assertEqual(self.assert_pins()['openssl'], '3.5.10')

    def test_legacy_multiassignment_dotnet_root(self):
        self.roots(True, True)
        self.change('cs/build.sh', '    DOTNET_ROOT=/cache/dotnet/$sdk_version',
                    'export DOTNET_ROOT=/cache/dotnet/$sdk_version NUGET_PACKAGES=/cache/nuget')
        self.assert_pins()

    def test_unknown_or_missing_cache_roots_rejected(self):
        for name, var in (('cs', 'cache'), ('openssl', 'cache_root'), ('codecs', 'cache_root')):
            p = self.repo / 'android' / name / 'build.sh'
            original = p.read_text()
            for expr in ('${OTHER_CACHE:-/cache}', '${S2A_BUILD_CACHE:-/other}',
                         '$(choose_cache)', '$unknown', '/other'):
                with self.subTest(name=name, expr=expr):
                    p.write_text(original.replace(
                        var + '=${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}', var + '=' + expr))
                    with self.assertRaises(ValueError): d.requirements(self.repo)
            p.write_text(original.replace(var + '=${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}\n', ''))
            with self.assertRaises(ValueError): d.requirements(self.repo)
            p.write_text(original)

    def test_reassigned_cache_and_version_sources_rejected(self):
        for name, var, value in (
                ('cs/build.sh', 'cache', '${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}'),
                ('openssl/build.sh', 'cache_root', '${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}'),
                ('codecs/build.sh', 'cache_root', '${S2A_BUILD_CACHE:-$HOME/.cache/s2arm-build}'),
                ('cs/build.sh', 'DOTNET_ROOT', '$cache/dotnet/$sdk_version'),
                ('openssl/build.sh', 'cache', '$cache_root/openssl/$version-ndk29-api$api'),
                ('codecs/opus-build.sh', 'target', '$cache_root/codecs/opus-$opus_ver-$key'),
                ('cs/build.sh', 'sdk_version', '10.0.403'),
                ('openssl/build.sh', 'version', '3.5.9')):
            p = self.repo / 'android' / name
            original = p.read_text()
            for extra in (var + '=' + value, 'export ' + var + '=' + value, 'true; ' + var + '=' + value):
                with self.subTest(name=name, extra=extra):
                    p.write_text(original + '\n' + extra + '\n')
                    with self.assertRaises(ValueError): d.requirements(self.repo)
            p.write_text(original)

    def test_unsafe_or_unknown_cache_expressions_rejected(self):
        for expr in ('$cache_root/../openssl/$version', '$cache_root//openssl/$version',
                     '$cache_root/openssl/$unknown', '$cache_root/openssl/$(choose_version)',
                     '$cache_root/openssl/${version:-3.5.9}', "'$cache_root/openssl/$version'",
                     '$cache_root/openssl/$version;false', '$cache_root/openssl/$version ; false'):
            p = self.repo / 'android/openssl/build.sh'
            original = p.read_text()
            with self.subTest(expr=expr):
                p.write_text(original.replace(
                    'cache=$cache_root/openssl/$version-ndk$S2A_ANDROID_NDK-api$api-$recipe_sha',
                    'cache=' + expr))
                with self.assertRaises(ValueError): d.requirements(self.repo)
            p.write_text(original)

    def test_wrong_pinned_sdk_path_and_versions_rejected(self):
        for name, before, after in (
                ('cs/build.sh', 'DOTNET_ROOT=$cache/dotnet/$sdk_version', 'DOTNET_ROOT=$cache/dotnet/10.0.999'),
                ('cs/build.sh', 'sdk_version=10.0.403', 'sdk_version=10.0.999'),
                ('openssl/build.sh', 'version=3.5.9', 'version=unknown'),
                ('tools/jdk17.sh', r"grep -q ' 17\.'", r"grep -q ' 21\.'")):
            p = self.repo / 'android' / name
            original = p.read_text()
            with self.subTest(name=name):
                self.change(name, before, after)
                with self.assertRaises(ValueError): d.requirements(self.repo)
            p.write_text(original)

    def test_unknown_ambiguous_or_missing_jdk_helper_rejected(self):
        p = self.repo / 'android/tools/jdk17.sh'
        original = p.read_text()
        for text in (original.replace(r"grep -q ' 17\.'", 'check_jdk'),
                     original + "\ngrep -q ' 21\\.'\n", ''):
            p.write_text(text)
            with self.assertRaises(ValueError): d.requirements(self.repo)
        p.unlink()
        with self.assertRaises((OSError, ValueError)): d.requirements(self.repo)

    def test_unknown_or_ambiguous_jdk_dispatch_rejected(self):
        p = self.repo / 'android/host/hostenv.sh'
        original = p.read_text()
        for text in (original.replace('bash "$hostenv_android_dir/tools/jdk17.sh"', 'bash "$other/tools/jdk17.sh"'),
                     original + '\nJAVAC=/other/javac\n',
                     original + '\ncase "$jv" in *" 17."*) ;; esac\n'):
            p.write_text(text)
            with self.assertRaises(ValueError): d.requirements(self.repo)

    def test_host_prepare_or_gate_layout_drift_rejected(self):
        for name, before in (('host/prepare.sh', '--slot'),
                             ('host/prepare.sh', '--prune'),
                             ('host/prepare.sh', 'rm -f "$receipt_path"'),
                             ('host/gate-apk.sh', 'deny network*'),
                             ('host/gate-apk.sh', 'touch "$receipt_path"')):
            with self.subTest(name=name, before=before):
                self.change(name, before, 'S2A_MUTATED_PIN')
                with self.assertRaises(ValueError): d.requirements(self.repo)

    def test_darwin_backend_selection_drift_rejected(self):
        p = self.repo / 'pack/targets/android/package.py'
        original = p.read_text()
        for text in (original.replace("machine != 'arm64'", "False"),
                     original.replace('未验证', 'unvalidated'),
                     original.replace("        return 'docker'", "        return 'host'")):
            p.write_text(text)
            with self.assertRaises(ValueError): d.requirements(self.repo)
        p.write_text(original)


if __name__ == '__main__':
    unittest.main()
