"""组装完整便携包和对应构建源码；不读取本机运行目录中的配置。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

repo = Path(__file__).resolve().parents[1]
version = (repo / 'deskflow-master/cmake/DeskSendVersion.cmake').read_text(encoding='utf-8').split('"')[1]


def hash_file(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def commit_id(ref):
    return subprocess.check_output(
        ['git', 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}'], cwd=repo, text=True
    ).strip()


def write_provenance():
    parser = argparse.ArgumentParser()
    parser.add_argument('platform', choices=('windows', 'macos', 'linux'))
    parser.add_argument('arch', choices=('x64', 'arm64'))
    parser.add_argument('output', type=Path)
    parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args(sys.argv[2:])
    hashes = {path.name: hash_file(path) for path in args.files}
    if len(hashes) != len(args.files):
        parser.error('Duplicate artifact filenames')
    record = dict(commit=commit_id('HEAD'), version=version, platform=args.platform, arch=args.arch, sha256=hashes)
    args.output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')


def member_parts(name):
    parts = name.rstrip('/').split('/')
    if '\\' in name or ':' in name or any(part in ('', '.', '..') for part in parts):
        raise ValueError(f'Invalid archive path: {name}')
    return tuple(parts)


def check_tar(path, platform):
    members = {}
    with tarfile.open(path, 'r:gz') as archive:
        for member in archive:
            name = member.name.removeprefix('./')
            if name in ('', '.') and member.isdir():
                continue
            parts = member_parts(name)
            if platform == 'macos' and parts[0] != 'DeskSend.app':
                raise ValueError(f'Unexpected macOS tar member: {name}')
            if parts in members or not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError(f'Invalid or duplicate tar member: {name}')
            members[parts] = member
    for parts, member in members.items():
        if any(not members[parts[:i]].isdir() for i in range(1, len(parts)) if parts[:i] in members):
            raise ValueError(f'Tar path descends through a non-directory: {member.name}')
        if not member.issym():
            continue
        resolved, followed = list(parts[:-1]), {parts}
        link = member.linkname
        pending = link.split('/')
        if not link or link.startswith('/') or '\\' in link or ':' in link:
            raise ValueError(f'Invalid tar link: {member.name}')
        while pending:
            part = pending.pop(0)
            if part == '..':
                if len(resolved) <= (1 if platform == 'macos' else 0):
                    raise ValueError(f'Escaping tar link: {member.name}')
                resolved.pop()
            elif part not in ('', '.'):
                resolved.append(part)
                key = tuple(resolved)
                entry = members.get(key)
                if entry and entry.issym():
                    link = entry.linkname
                    if key in followed or not link or link.startswith('/') or '\\' in link or ':' in link:
                        raise ValueError(f'Invalid tar link chain: {member.name}')
                    followed.add(key)
                    resolved.pop()
                    pending = link.split('/') + pending
    prefix = ('DeskSend.app', 'Contents', 'MacOS') if platform == 'macos' else ('bin',)
    for exe in ('DeskSend' if platform == 'macos' else 'deskflow', 'deskflow-core', 'localsend-cli'):
        member = members.get(prefix + (exe,))
        if not member or not member.isfile() or not member.size or not member.mode & 0o111:
            raise ValueError(f'Missing executable: {"/".join(prefix + (exe,))}')


if sys.argv[1:2] == ['--write-provenance']:
    write_provenance()
    sys.exit(0)

parser = argparse.ArgumentParser()
parser.add_argument('--deskflow-zip', required=True, type=Path)
parser.add_argument('--deskflow-msi', type=Path)
parser.add_argument('--localsend-exe', required=True, type=Path)
parser.add_argument('--macos-tar', type=Path)
parser.add_argument('--linux-tar', type=Path)
parser.add_argument('--build-commit', required=True)
parser.add_argument('--allow-missing-provenance', action='store_true')
parser.add_argument('--out', required=True, type=Path)
args = parser.parse_args()
installer_name = f'DeskSend-{version}-windows-x64.msi'
if args.deskflow_msi and (args.deskflow_msi.name != installer_name or not args.deskflow_msi.is_file()):
    parser.error(f'--deskflow-msi 必须指向 {installer_name}')
for label, tar_path in (('--macos-tar', args.macos_tar), ('--linux-tar', args.linux_tar)):
    if tar_path and not tar_path.is_file():
        parser.error(f'{label} 指向的文件不存在：{tar_path}')
build_commit = commit_id(args.build_commit)
source_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
subprocess.run(['git', 'diff', '--exit-code', build_commit, source_commit, '--', 'deskflow-master', 'localsend-main'], cwd=repo, check=True)
for path, platform, arch in (
    (args.deskflow_zip, 'windows', 'x64'), (args.deskflow_msi, 'windows', 'x64'),
    (args.localsend_exe, 'windows', 'x64'), (args.macos_tar, 'macos', 'arm64'),
    (args.linux_tar, 'linux', 'x64'),
):
    if path is None:
        continue
    sidecar = path.with_name('BUILD_PROVENANCE.json')
    if sidecar.is_file():
        record = json.loads(sidecar.read_text(encoding='utf-8-sig'))
        expected = dict(commit=build_commit, version=version, platform=platform, arch=arch)
        if any(record.get(key) != value for key, value in expected.items()) or (
            record.get('sha256', {}).get(path.name) != hash_file(path)
        ):
            parser.error(f'Artifact provenance does not match: {path}')
    elif not args.allow_missing_provenance:
        parser.error(f'Missing BUILD_PROVENANCE.json next to {path}')
    if platform != 'windows':
        check_tar(path, platform)
args.out.mkdir(parents=True, exist_ok=True)
root = args.out / f'DeskSend-{version}-windows-x64'
if root.exists():
    raise SystemExit(f'输出目录已经存在，请使用新的输出路径：{root}')
root.mkdir()
with zipfile.ZipFile(args.deskflow_zip) as archive:
    entries = {}
    roots = set()
    spellings = {}
    for info in archive.infolist():
        parts = member_parts(info.filename)
        roots.add(parts[0])
        for part in parts:
            stem = part.split('.')[0].rstrip().upper()
            if (part.endswith((' ', '.')) or re.fullmatch(r'CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9¹²³]', stem)
                    or any(ord(char) < 32 or char in '<>"|?*' for char in part)):
                raise ValueError(f'Invalid Windows archive path: {info.filename}')
        key = tuple(part.casefold() for part in parts)
        if key in entries or stat.S_ISLNK(info.external_attr >> 16) or (len(parts) == 1 and not info.is_dir()):
            raise ValueError(f'Invalid or duplicate ZIP member: {info.filename}')
        entries[key] = (info, parts)
        for i in range(1, len(parts) + 1):
            if spellings.setdefault(key[:i], parts[:i]) != parts[:i]:
                raise ValueError(f'Conflicting ZIP path casing: {info.filename}')
    if len(roots) != 1:
        raise ValueError('Windows ZIP must have exactly one root directory')
    for key, (info, parts) in entries.items():
        if any(not entries[key[:i]][0].is_dir() for i in range(1, len(key)) if key[:i] in entries):
            raise ValueError(f'ZIP path descends through a file: {info.filename}')
        if info.is_dir():
            continue
        target = root.joinpath(*parts[1:])
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(info) as source, target.open('xb') as output:
            shutil.copyfileobj(source, output)
shutil.copy2(args.localsend_exe, root / 'localsend-cli.exe')
for name in ('DeskSend.exe', 'deskflow-core.exe', 'localsend-cli.exe'):
    if not (root / name).is_file():
        raise ValueError(f'完整包缺少 {name}')
settings = root / 'settings'
settings.mkdir(exist_ok=True)
if any(p.is_file() and p.name != 'Deskflow.conf' for p in settings.rglob('*')):
    raise ValueError('构建产物中混入了本机配置或证书')
(settings / 'Deskflow.conf').write_text('[gui]\nenableUpdateCheck=true\n[core]\nlanguage=zh_CN\n', encoding='utf-8')
(root / 'README.txt').write_text('''DeskSend {version} Windows x64 便携版

解压完整目录，运行 DeskSend.exe。两台电脑均使用此完整包。
DeskSend 已整合键鼠共享与局域网文件传输，无需单独安装 LocalSend。
帮助 → 检查 DeskSend 更新，可下载完整更新包并安装重启。
英文入口：Help → Check for DeskSend Updates...。安装前请等待文件传输结束。
更新会保留 settings 目录中的配置、证书和配对。
安装失败时查看提示中的更新日志，旧程序备份在 .desksend-backups 中。

从旧版手动迁移：先等传输结束并退出旧程序，再将旧目录的整个 settings 目录复制到新目录，
覆盖新包的默认 settings；运行 DeskSend.exe。不要只复制其中一个 exe。

使用主机共享的鼠标，可从主机拖文件到副机，也可从副机拖回主机。
多台副机连接同一主机时，可拖着文件穿过主机屏幕，到另一台副机松手。
拖入目标屏幕显示松手接收提示，松手后显示准备接收、进度和结果。
源文件保留；松手前按 Esc 或拖回源屏可取消，传输中可在任一端进度窗点击“取消传输”，空目录不传输。

主机监听 TCP 24800；双向跨屏文件传输时，主机与副机均需放行入站 TCP 53318。
需要控制副机的 UAC 提示和 Windows 登录界面时，可在副机安装同一发布页的 MSI 并开启后台服务。
标题栏关闭按钮默认收进系统托盘。要关闭键鼠核心并退出 DeskSend，请从托盘菜单选择“退出”。

使用指南：https://github.com/wtyLaziness/desksend/blob/main/docs/使用指南.md
更新和对应源码：https://github.com/wtyLaziness/desksend-releases/releases
DeskSend 基于 Deskflow（GPLv2 + OpenSSL exception）及 LocalSend（Apache-2.0）。
版权和许可证随包附带，对应构建源码作为同一 Release 附件提供。
'''.format(version=version), encoding='utf-8')
shutil.copy2(repo / 'localsend-main/LICENSE', root / 'LocalSend-LICENSE.txt')
shutil.copytree(repo / 'deskflow-master/LICENSES', root / 'LICENSES', dirs_exist_ok=True)
shutil.copy2(repo / 'deskflow-master/LICENSE', root / 'Deskflow-LICENSE.txt')
shutil.copy2(repo / 'THIRD_PARTY_NOTICES.md', root / 'THIRD_PARTY_NOTICES.md')
files = {p.relative_to(root).as_posix(): hash_file(p) for p in sorted(root.rglob('*')) if p.is_file() and 'settings' not in p.relative_to(root).parts}
manifest = {'format': 1, 'product': 'DeskSend', 'version': version, 'build_commit': build_commit, 'source_commit': source_commit, 'files': files}
(root / 'desksend-build.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
binary_zip = args.out / (root.name + '.zip')
with zipfile.ZipFile(binary_zip, 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(args.out))
source_zip = args.out / f'DeskSend-{version}-source.zip'
with tempfile.TemporaryDirectory(prefix='.desksend-source-', dir=args.out) as temp:
    candidate = Path(temp) / source_zip.name
    subprocess.run(['git', 'archive', '--format=zip', f'--prefix=DeskSend-{version}-source/',
                    '-o', str(candidate.resolve()), source_commit], cwd=repo, check=True)
    with zipfile.ZipFile(candidate) as archive:
        forbidden = [name for name in archive.namelist()
                     if name.lower().endswith(('.pem', '.key', '.log', '.pfx', '.p12'))
                     or '/.git/' in name.lower() or '/settings/deskflow.conf' in name.lower()]
        if forbidden:
            raise ValueError(f'源码包包含不应公开的文件：{forbidden}')
    candidate.replace(source_zip)
release_assets = [binary_zip, source_zip]
if args.deskflow_msi:
    installer = args.out / installer_name
    shutil.copy2(args.deskflow_msi, installer)
    release_assets.append(installer)
if args.macos_tar:
    macos_asset = args.out / f'DeskSend-{version}-macos-arm64.tar.gz'
    shutil.copy2(args.macos_tar, macos_asset)
    release_assets.append(macos_asset)
if args.linux_tar:
    linux_asset = args.out / f'DeskSend-{version}-linux-x64.tar.gz'
    shutil.copy2(args.linux_tar, linux_asset)
    release_assets.append(linux_asset)
(args.out / 'SHA256SUMS.txt').write_text(''.join(f'{hash_file(p)}  {p.name}\n' for p in release_assets), encoding='utf-8')
print(json.dumps({'binary': str(binary_zip), 'source': str(source_zip), 'version': version, 'files': len(files), 'assets': [p.name for p in release_assets]}, ensure_ascii=False))
