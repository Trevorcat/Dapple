"""Download a LOCAL evaluation subset of MIT-Adobe FiveK (research licence: local evaluation only, never redistribute, never train
a shipped model on it). N images evenly spaced over the ones the dataset page links individually: the DNG plus the five experts'
sRGB thumbnails (img/thmb_a..e, 100 px long edge). The 16-bit expert TIFFs (~32 MB each) were not used: the server delivers ~50 KB/s
per connection from this network. Into build/assistant/fivek/{dng,thmb_a..e}/. Usage: python -I experiments/assistant/fivek_dl.py [N=100]"""
import os, re, sys, urllib.request, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'build', 'assistant', 'fivek')
BASE = 'https://data.csail.mit.edu/graphics/fivek/'
EXPERTS = 'abcde'


def get(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0: return True
    for a in range(6):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'curl-client'})
            with urllib.request.urlopen(req, timeout=300) as r, open(path + '.part', 'wb') as f:
                while True:
                    b = r.read(1 << 16)
                    if not b: break
                    f.write(b)
            os.replace(path + '.part', path); return True
        except Exception as e:
            print('retry', a, url, e, flush=True); time.sleep(5)
    return False


def one(nm):
    ok = all(get(BASE + f'img/thmb_{x}/{nm}.jpg', os.path.join(OUT, 'thmb_' + x, nm + '.jpg')) for x in EXPERTS)
    ok &= get(BASE + f'img/dng/{nm}.dng', os.path.join(OUT, 'dng', nm + '.dng'))
    print(nm, 'ok' if ok else 'FAIL', flush=True)


def main(n):
    html = None
    for a in range(6):
        try:
            html = urllib.request.urlopen(BASE, timeout=300).read().decode('latin1'); break
        except Exception as e:
            print('index retry', a, e, flush=True); time.sleep(5)
    names = sorted(set(re.findall(r'href="img/dng/([^"]+)\.dng"', html)))
    pick = [names[round(i * (len(names) - 1) / (n - 1))] for i in range(n)] if n < len(names) else names
    for d in ['dng'] + ['thmb_' + x for x in EXPERTS]: os.makedirs(os.path.join(OUT, d), exist_ok=True)
    with open(os.path.join(OUT, 'subset.txt'), 'w') as f: f.write('\n'.join(pick) + '\n')
    for lic in ('LicenseAdobe.txt', 'LicenseAdobeMIT.txt', 'filesAdobe.txt', 'filesAdobeMIT.txt'):
        get(BASE + 'legal/' + lic, os.path.join(OUT, lic))
    with ThreadPoolExecutor(6) as ex: list(ex.map(one, pick))
    print('DONE', flush=True)


if __name__ == '__main__':
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 100)
