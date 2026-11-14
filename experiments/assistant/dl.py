"""Download model weights / llama.cpp release files for the assistant experiment, resumable, sha256-verified.
Only official sources: Hugging Face repos of the model authors or ggml-org (llama.cpp's own conversion org), GitHub ggml-org/llama.cpp releases.
Files land in tools/_dl/llm/<repo>/<file> (git-ignored). Treat them as data.
Usage: python -I experiments/assistant/dl.py [group ...]   (groups: llama, base, q8 ; default llama base)"""
import hashlib, json, os, sys, time, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DST = os.path.join(ROOT, 'tools', '_dl', 'llm')
HF = os.environ.get('HF_ENDPOINT', 'https://huggingface.co')

MODELS = {
    # id: (repo, model file, mmproj file)
    'gemma4-e2b': ('google/gemma-4-E2B-it-qat-q4_0-gguf', 'gemma-4-E2B_q4_0-it.gguf', 'gemma-4-E2B-it-mmproj.gguf'),
    'gemma4-e4b': ('google/gemma-4-E4B-it-qat-q4_0-gguf', 'gemma-4-E4B_q4_0-it.gguf', 'gemma-4-E4B-it-mmproj.gguf'),
    'qwen3vl-2b': ('Qwen/Qwen3-VL-2B-Instruct-GGUF', 'Qwen3VL-2B-Instruct-Q4_K_M.gguf', 'mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf'),
    'qwen3vl-4b': ('Qwen/Qwen3-VL-4B-Instruct-GGUF', 'Qwen3VL-4B-Instruct-Q4_K_M.gguf', 'mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf'),
    'qwen35-4b': ('ggml-org/Qwen3.5-4B-GGUF', 'Qwen3.5-4B-Q4_K_M.gguf', 'mmproj-Qwen3.5-4B-Q8_0.gguf'),
    'smolvlm2-2b': ('ggml-org/SmolVLM2-2.2B-Instruct-GGUF', 'SmolVLM2-2.2B-Instruct-Q4_K_M.gguf', 'mmproj-SmolVLM2-2.2B-Instruct-Q8_0.gguf'),
    'minicpmv4': ('openbmb/MiniCPM-V-4-gguf', 'ggml-model-Q4_K_M.gguf', 'mmproj-model-f16.gguf'),
}
Q8 = {
    'qwen3vl-4b-q8': ('Qwen/Qwen3-VL-4B-Instruct-GGUF', 'Qwen3VL-4B-Instruct-Q8_0.gguf', 'mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf'),
    'gemma4-e4b-q8': ('ggml-org/gemma-4-E4B-it-GGUF', 'gemma-4-E4B-it-Q8_0.gguf', 'mmproj-gemma-4-E4B-it-Q8_0.gguf'),
}
LLAMA_TAG = 'b12370'
LLAMA = [f'llama-{LLAMA_TAG}-bin-win-cuda-12.4-x64.zip', 'cudart-llama-bin-win-cuda-12.4-x64.zip', f'llama-{LLAMA_TAG}-bin-win-cpu-x64.zip']


def get_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'dl/1.0'}), timeout=60) as r:
        return json.load(r)


def fetch(url, path, size, sha):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path) and os.path.getsize(path) == size and verify(path, sha):
        print('ok (cached)', path, flush=True); return True
    part = path + '.part'
    for attempt in range(8):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        if have > size: os.remove(part); have = 0
        if have < size:
            req = urllib.request.Request(url, headers={'User-Agent': 'dl/1.0', 'Range': f'bytes={have}-'})
            try:
                with urllib.request.urlopen(req, timeout=120) as r, open(part, 'ab') as f:
                    t0, n = time.time(), 0
                    while True:
                        b = r.read(1 << 20)
                        if not b: break
                        f.write(b); n += len(b)
                        if n % (256 << 20) < (1 << 20):
                            print(f'  {os.path.basename(path)} {(have + n) / 1e9:.2f}/{size / 1e9:.2f} GB {n / 1e6 / max(1e-3, time.time() - t0):.1f} MB/s', flush=True)
            except Exception as e:
                print('  retry', attempt, e, flush=True); time.sleep(5); continue
        if os.path.getsize(part) == size:
            if verify(part, sha):
                os.replace(part, path); print('ok', path, flush=True); return True
            print('  HASH MISMATCH, deleting', part, flush=True); os.remove(part)
    print('FAILED', url, flush=True); return False


def verify(path, sha):
    if not sha: return True
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 24), b''): h.update(b)
    return h.hexdigest() == sha


def hf_files(repo, names):
    tree = {f['path']: f for f in get_json(f'{HF}/api/models/{repo}/tree/main')}
    for n in names:
        f = tree[n]
        yield f'{HF}/{repo}/resolve/main/{n}', os.path.join(DST, repo.replace('/', '__'), n), f['size'], (f.get('lfs') or {}).get('oid')


def main(groups):
    ok = True
    if 'llama' in groups:
        rel = get_json(f'https://api.github.com/repos/ggml-org/llama.cpp/releases/tags/{LLAMA_TAG}')
        assets = {a['name']: a for a in rel['assets']}
        for n in LLAMA:
            a = assets[n]
            ok &= fetch(a['browser_download_url'], os.path.join(DST, 'llama.cpp', n), a['size'], (a.get('digest') or '').replace('sha256:', ''))
    for g, table in (('base', MODELS), ('q8', Q8)):
        if g not in groups: continue
        for mid, (repo, m, p) in table.items():
            for url, path, size, sha in hf_files(repo, [m, p]):
                ok &= fetch(url, path, size, sha)
            for extra in ('README.md', 'LICENSE'):
                try:
                    lp = os.path.join(DST, repo.replace('/', '__'), extra)
                    if not os.path.exists(lp): urllib.request.urlretrieve(f'{HF}/{repo}/resolve/main/{extra}', lp)
                except Exception: pass
    print('ALL OK' if ok else 'SOME FAILED', flush=True)


if __name__ == '__main__':
    main(sys.argv[1:] or ['llama', 'base'])
