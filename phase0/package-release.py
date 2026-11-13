"""组装完整便携包和对应构建源码；不读取本机运行目录中的配置。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--deskflow-zip', required=True, type=Path)
parser.add_argument('--localsend-exe', required=True, type=Path)
parser.add_argument('--build-commit', required=True)
parser.add_argument('--out', required=True, type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
version = (repo / 'deskflow-master/cmake/DeskSendVersion.cmake').read_text(encoding='utf-8').split('"')[1]
build_commit = subprocess.check_output(['git', 'rev-parse', args.build_commit], cwd=repo, text=True).strip()
source_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
subprocess.run(['git', 'diff', '--exit-code', build_commit, source_commit, '--', 'deskflow-master', 'localsend-main'], cwd=repo, check=True)
args.out.mkdir(parents=True, exist_ok=True)
root = args.out / f'DeskSend-{version}-windows-x64'
if root.exists():
    raise SystemExit(f'输出目录已经存在，请使用新的输出路径：{root}')
root.mkdir()
with zipfile.ZipFile(args.deskflow_zip) as archive:
    # CI 产物有一层版本目录，仅接受普通的相对路径。
    for info in archive.infolist():
        path = Path(*info.filename.replace('\\', '/').split('/')[1:])
        if not path.parts or info.is_dir():
            continue
        if path.is_absolute() or '..' in path.parts or ':' in str(path):
            raise ValueError('无效的构建产物路径')
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(info))
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
更新会保留 settings 目录中的配置、证书和配对。
安装失败时查看提示中的更新日志，旧程序备份在 .desksend-backups 中。

从旧版手动迁移：先退出旧程序，再将旧目录的整个 settings 目录复制到新目录，
覆盖新包的默认 settings；运行 DeskSend.exe。不要只复制其中一个 exe。

主机监听 TCP 24800；副机的文件接收组件监听 TCP 53318，需要防火墙放行。
便携版不包含系统服务，不能控制 UAC 提示和 Windows 登录界面。

更新和对应源码：https://github.com/wtyLaziness/desksend-releases/releases
DeskSend 基于 Deskflow（GPLv2 + OpenSSL exception）及 LocalSend（MIT）。
版权和许可证随包附带，对应构建源码作为同一 Release 附件提供。
'''.format(version=version), encoding='utf-8')
shutil.copy2(repo / 'localsend-main/LICENSE', root / 'LocalSend-LICENSE.txt')
shutil.copytree(repo / 'deskflow-master/LICENSES', root / 'LICENSES', dirs_exist_ok=True)
shutil.copy2(repo / 'deskflow-master/LICENSE', root / 'Deskflow-LICENSE.txt')
hash_file = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
files = {p.relative_to(root).as_posix(): hash_file(p) for p in sorted(root.rglob('*')) if p.is_file() and 'settings' not in p.relative_to(root).parts}
manifest = {'format': 1, 'product': 'DeskSend', 'version': version, 'build_commit': build_commit, 'source_commit': source_commit, 'files': files}
(root / 'desksend-build.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
binary_zip = args.out / (root.name + '.zip')
with zipfile.ZipFile(binary_zip, 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(args.out))
source_zip = args.out / f'DeskSend-{version}-source.zip'
subprocess.run(['git', 'archive', '--format=zip', f'--prefix=DeskSend-{version}-source/', '-o', str(source_zip.resolve()), source_commit], cwd=repo, check=True)
with zipfile.ZipFile(source_zip) as archive:
    forbidden = [name for name in archive.namelist() if name.lower().endswith(('.pem', '.key', '.log')) or '/.git/' in name or '/settings/Deskflow.conf' in name]
    if forbidden:
        raise ValueError(f'源码包包含不应公开的文件：{forbidden}')
(args.out / 'SHA256SUMS.txt').write_text(''.join(f'{hash_file(p)}  {p.name}\n' for p in (binary_zip, source_zip)), encoding='utf-8')
print(json.dumps({'binary': str(binary_zip), 'source': str(source_zip), 'version': version, 'files': len(files)}, ensure_ascii=False))
