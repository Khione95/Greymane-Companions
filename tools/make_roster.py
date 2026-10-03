"""The Greymanes the plugin may make follow: the camp comrades (the 38 the
game hires through its scout missions) and the story Greymanes of
build_test.py. Animals, mounts, Kliff, Damiane and Oongka are never in it.

Writes asi/src/roster.h (character keys and the names the game shows for the log).
Usage: python make_roster.py"""
import os

import build_test
import display_names  # noqa: E402
import tables

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'asi', 'src', 'roster.h')


def short_name(name):
    """NHW_Unique_Matilda_634_1 -> Matilda."""
    return name.split('_Unique_', 1)[-1].split('_')[0]


def main():
    chars = tables.Table('characterinfo')
    shown = display_names.display_names(chars, build_test.camp_comrades(chars) + [k for k, _, _ in build_test.TESTS])
    keys = build_test.camp_comrades(chars) + [k for k, _, _ in build_test.TESTS]
    lines = [
        '#pragma once',
        '',
        '// Made by tools/make_roster.py: the camp comrades and the story Greymanes.',
        '',
        '#include <stdint.h>',
        '',
        'struct Greymane { uint32_t key; const char* name; };',
        '',
        'static const Greymane ROSTER[] = {',
    ]
    for key in keys:
        full = chars.names.get(key, '?')
        lines.append(f'    {{ 0x{key:06X}, "{shown.get(key) or short_name(full)}" }},  // {full}')
    lines += ['};', '']
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    print(f'{len(keys)} Greymanes -> {os.path.normpath(OUT)}')


if __name__ == '__main__':
    main()
