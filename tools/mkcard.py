"""Build a virtual SD card per game: cards/<slug>/PALM/Programs/<data folder> (hardlinked files).
- directories named '<name>-<CRID>' or anything under a 'Programs' directory are copied as-is;
- loose data files next to a TNA PRC go to that PRC's data directory '<dbname>-<creator>'
  (or the path in its 'Twdp' #0 resource)."""
import os, re, shutil, sys
sys.path.insert(0, 'tools'); import prc

SKIP_EXT = {'.prc', '.pdb', '.txt', '.htm', '.html', '.pdf', '.exe', '.ini', '.zip', '.png', '.pcx', '.url'}


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        return
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def link_tree(src, dst):
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        for f in files:
            link(os.path.join(root, f), os.path.normpath(os.path.join(dst, rel, f)))


def data_dir(prc_path):
    try:
        p = prc.parse(open(prc_path, 'rb').read())
    except Exception:
        return None
    if not p['res']:
        return None
    for t, i, o, d in p['ents']:
        if t == 'Twdp' and i == 0:
            s = d.split(b'\0')[0].decode('latin1').replace(chr(92), '/').strip('/')
            return s
    if not any(t in ('ARMZ', 'ARMC') and i == 1 for t, i, o, d in p['ents']):
        return None
    suffix = '-' + p['creator']
    return 'PALM/Programs/' + p['name'] + ('' if p['name'].endswith(suffix) else suffix)


def main():
    for slug in sorted(os.listdir('games')):
        base = os.path.join('games', slug)
        card = os.path.join('cards', slug)
        done = set()
        for root, dirs, files in os.walk(base):
            for d in list(dirs):
                full = os.path.join(root, d)
                if d.lower() == 'programs':
                    for sub in os.listdir(full):
                        link_tree(os.path.join(full, sub), os.path.join(card, 'PALM', 'Programs', sub))
                        print(f'{slug:24} Programs/{sub}')
                    dirs.remove(d)
                elif re.match(r'.+-[A-Za-z0-9]{4}$', d):
                    link_tree(full, os.path.join(card, 'PALM', 'Programs', d))
                    print(f'{slug:24} {d}')
                    dirs.remove(d)
            for f in files:
                if not f.lower().endswith('.prc'):
                    continue
                dd = data_dir(os.path.join(root, f))
                if not dd or (root, dd) in done:
                    continue
                done.add((root, dd))
                loose = [x for x in files if os.path.splitext(x)[1].lower() not in SKIP_EXT]
                for x in loose:
                    link(os.path.join(root, x), os.path.join(card, *dd.split('/'), x))
                if loose:
                    print(f'{slug:24} {dd}  <- {len(loose)} loose files')


if __name__ == '__main__':
    main()
