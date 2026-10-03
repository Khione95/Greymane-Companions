"""Greymane Companions release: dist/Greymane Companions <version>.zip

  Greymane Companions/                 the mod (the zip's only folder)
    GreymaneCompanions.asi             the plugin; DMM puts it into bin64
    Greymane Companions.json           the talk options (DMM v3 JSON, from
                                       build_test4.py's records)
    README.txt, THIRD_PARTY.txt

A JSON beside the asi keeps DMM 3.x from taking the mod down its plugin import
(as Character Creator's). Build the plugin (Release x64) first. Checks the
plugin for this PC's user name before packing.
Usage: python make_release.py"""
import json
import os
import re
import shutil
import zipfile

import build_test4

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
VERSION = '1.0'
NAME = 'Greymane Companions'


def main():
    asi = os.path.join(ROOT, 'asi', 'bin', 'GreymaneCompanions.asi')
    data = open(asi, 'rb').read()
    user = os.environ.get('USERNAME', '').encode()
    if user and re.search(re.escape(user), data, re.I):
        raise SystemExit('the plugin holds the user name - rebuild it (PDBALTPATH)')
    version = re.search(rb'#define GREYMANE_VERSION "([^"]+)"', open(os.path.join(ROOT, 'asi', 'src', 'game.h'), 'rb').read())
    if not version or version.group(1).decode() != VERSION:
        raise SystemExit('game.h GREYMANE_VERSION is not ' + VERSION)

    mod, _ = build_test4.build()
    mod['modinfo'] = {
        'author': 'Khione', 'title': NAME, 'version': VERSION,
        'description': 'Recruit the Greymanes from your camp as companions: the Recruit and Dismiss talk options on '
                       'the camp comrades (with GreymaneCompanions.asi), and the game\'s Follow and Standby.',
    }

    stage = os.path.join(ROOT, 'dist', 'stage')
    if os.path.exists(stage):
        shutil.rmtree(stage)
    package = os.path.join(stage, NAME)
    os.makedirs(package)
    shutil.copy2(asi, os.path.join(package, 'GreymaneCompanions.asi'))
    with open(os.path.join(package, NAME + '.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(mod, f, indent=2)
    for doc in ('README.txt', 'THIRD_PARTY.txt'):
        shutil.copy2(os.path.join(ROOT, 'release', doc), package)

    archive = os.path.join(ROOT, 'dist', f'{NAME} {VERSION}.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for folder, _, files in os.walk(stage):
            for name in sorted(files):
                path = os.path.join(folder, name)
                z.write(path, os.path.relpath(path, stage).replace(os.sep, '/'))
    shutil.rmtree(stage)
    print(f'{os.path.normpath(archive)} ({os.path.getsize(archive) // 1024} KB)')


if __name__ == '__main__':
    main()
