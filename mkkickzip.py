#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make or unpack the Kickstart zip archive that BBoot boots from.

  mkkickzip.py [-o Kickstart.zip] [-m FILE]... SOURCE
  mkkickzip.py -x Kickstart.zip [DIR]

SOURCE is an AmigaOS Kickstart directory, such as SYS:Kickstart copied
from an AmigaOS 4.1 installation, a directory that contains one, or a
Kickstart zip archive to make a new one from.

BBoot boots the first configuration (LABEL) of Kickstart/Kicklayout and
finds its files by exact name, whereas AmigaOS file names ignore case.
So every file named in that configuration must exist, and is stored
under the spelling the Kicklayout uses.  Other files are stored as they
are.

-m FILE adds a module: FILE is stored in Kickstart/, and the first
configuration loads it, by enabling its commented-out MODULE line or by
adding one.  For example, booting from QEMU's lsi53c895a SCSI controller
on the A1222 needs

  mkkickzip.py -o Kickstart-scsi.zip -m lsi53c8xx.device.kmod \\
      -m nvram.config.kmod Kickstart-a1222.zip

-x unpacks a Kickstart zip archive into DIR, by default the current
directory.
"""

import argparse
import os
import re
import sys
import zipfile

ENTRY = re.compile(r'^(;?)\s*(EXEC|MODULE)\s+(\S+)', re.I)


def die(msg):
    sys.exit('mkkickzip: ' + msg)


def read_source(src):
    """Return {name: data} for the files of the Kickstart directory, with
    names relative to it."""
    files = {}
    if zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            for i in z.infolist():
                if i.is_dir():
                    continue
                parts = i.filename.split('/')
                if len(parts) != 2 or parts[0].lower() != 'kickstart':
                    die('%s: %s is not in Kickstart/' % (src, i.filename))
                files[parts[1]] = z.read(i)
        return files
    if not os.path.isdir(src):
        die('%s is neither a directory nor a zip archive' % src)
    names = {n.lower(): n for n in os.listdir(src)}
    if 'kicklayout' not in names and 'kickstart' in names:
        src = os.path.join(src, names['kickstart'])
    for n in sorted(os.listdir(src)):
        p = os.path.join(src, n)
        if os.path.isdir(p):
            continue
        if n.startswith('.') or n.endswith('.info'):
            continue
        with open(p, 'rb') as f:
            files[n] = f.read()
    return files


def first_config(lines):
    """Return the indices of the lines of the first configuration: from
    its LABEL line up to the empty line that ends it."""
    start = next((i for i, l in enumerate(lines)
                  if l.split()[:1] and l.split()[0].upper() == 'LABEL'), None)
    if start is None:
        die('Kicklayout has no LABEL line')
    end = start + 1
    while end < len(lines) and lines[end].strip():
        end += 1
    return start, end


def add_modules(layout, modules):
    lines = layout.split('\n')
    for m in modules:
        start, end = first_config(lines)
        path = 'Kickstart/' + m
        for i in range(start, end):
            e = ENTRY.match(lines[i])
            if e and e.group(2).upper() == 'MODULE' and \
                    e.group(3).lower() == path.lower():
                if e.group(1):
                    lines[i] = 'MODULE ' + e.group(3)
                break
        else:
            lines.insert(end, 'MODULE ' + path)
    return '\n'.join(lines)


def make(args):
    files = read_source(args.source)
    for m in args.module:
        with open(m, 'rb') as f:
            files[os.path.basename(m)] = f.read()
    lower = {}
    for n in files:
        if n.lower() in lower:
            die('%s and %s differ only in case' % (lower[n.lower()], n))
        lower[n.lower()] = n
    if 'kicklayout' not in lower:
        die('%s has no Kicklayout' % args.source)
    layout = files.pop(lower.pop('kicklayout')).decode('latin-1')
    if args.module:
        layout = add_modules(layout, [os.path.basename(m)
                                      for m in args.module])

    lines = layout.split('\n')
    start, end = first_config(lines)
    print('Booting "%s"' % lines[start].split(None, 1)[1].strip())
    out = {'Kicklayout': layout.encode('latin-1')}
    missing = []
    for l in lines[start + 1:end]:
        e = ENTRY.match(l)
        if not e or e.group(1):
            continue
        path = e.group(3)
        if not path.lower().startswith('kickstart/') or path.count('/') != 1:
            die('%s is not in Kickstart/' % path)
        name = path.split('/', 1)[1]
        if name.lower() not in lower:
            missing.append(name)
            continue
        out[name] = files.pop(lower.pop(name.lower()))
    if missing:
        die('the first configuration loads files that are missing: ' +
            ', '.join(missing))
    out.update(files)

    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo('Kickstart/'), b'')
        for n, data in out.items():
            z.writestr('Kickstart/' + n, data)
    print('%s: %d files' % (args.output, len(out)))


def extract(args):
    dest = args.source or '.'
    with zipfile.ZipFile(args.extract) as z:
        for n in z.namelist():
            if n.startswith('/') or '..' in n.split('/'):
                die('%s: unsafe name %s' % (args.extract, n))
        z.extractall(dest)
        print('%s: %d files' % (os.path.join(dest, 'Kickstart'),
                                len([n for n in z.namelist()
                                     if not n.endswith('/')])))


def main():
    p = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0],
        epilog=__doc__.split('\n\n', 2)[2],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-o', '--output', default='Kickstart.zip',
                   help='zip archive to make (default: Kickstart.zip)')
    p.add_argument('-m', '--module', action='append', default=[],
                   metavar='FILE', help='add and load a module')
    p.add_argument('-x', '--extract', metavar='ZIP',
                   help='unpack ZIP instead')
    p.add_argument('source', nargs='?',
                   help='Kickstart directory or zip archive; with -x, '
                   'the directory to unpack into')
    args = p.parse_args()
    if args.extract:
        if args.module:
            p.error('-m does not go with -x')
        extract(args)
    elif not args.source:
        p.error('no source given')
    else:
        make(args)


if __name__ == '__main__':
    main()
