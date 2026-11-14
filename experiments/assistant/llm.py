"""Local VLMs through llama-server (official llama.cpp release b12370, tools/_dl/llm/llama.cpp) and an optional cloud endpoint.
One server at a time; JSON-schema constrained output (llama.cpp turns the schema into a grammar), temperature 0 and a fixed seed by default."""
import base64, io, json, os, subprocess, time, urllib.request, urllib.error
import psutil
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LLM = os.path.join(ROOT, 'tools', '_dl', 'llm')
BIN = {'cuda': os.path.join(LLM, 'llama.cpp', 'cuda', 'llama-server.exe'), 'cpu': os.path.join(LLM, 'llama.cpp', 'cpu', 'llama-server.exe')}

MODELS = {
    'gemma4-e2b': ('google__gemma-4-E2B-it-qat-q4_0-gguf', 'gemma-4-E2B_q4_0-it.gguf', 'gemma-4-E2B-it-mmproj.gguf'),
    'gemma4-e4b': ('google__gemma-4-E4B-it-qat-q4_0-gguf', 'gemma-4-E4B_q4_0-it.gguf', 'gemma-4-E4B-it-mmproj.gguf'),
    'qwen3vl-2b': ('Qwen__Qwen3-VL-2B-Instruct-GGUF', 'Qwen3VL-2B-Instruct-Q4_K_M.gguf', 'mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf'),
    'qwen3vl-4b': ('Qwen__Qwen3-VL-4B-Instruct-GGUF', 'Qwen3VL-4B-Instruct-Q4_K_M.gguf', 'mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf'),
    'qwen35-4b': ('ggml-org__Qwen3.5-4B-GGUF', 'Qwen3.5-4B-Q4_K_M.gguf', 'mmproj-Qwen3.5-4B-Q8_0.gguf'),
    'smolvlm2-2b': ('ggml-org__SmolVLM2-2.2B-Instruct-GGUF', 'SmolVLM2-2.2B-Instruct-Q4_K_M.gguf', 'mmproj-SmolVLM2-2.2B-Instruct-Q8_0.gguf'),
    'minicpmv4': ('openbmb__MiniCPM-V-4-gguf', 'ggml-model-Q4_K_M.gguf', 'mmproj-model-f16.gguf'),
    'qwen3vl-4b-q8': ('Qwen__Qwen3-VL-4B-Instruct-GGUF', 'Qwen3VL-4B-Instruct-Q8_0.gguf', 'mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf'),
}


def available(mid):
    d, m, p = MODELS[mid]
    return os.path.exists(os.path.join(LLM, d, m)) and os.path.exists(os.path.join(LLM, d, p))


def jpeg_b64(rgb, long_edge=768, quality=88):
    im = Image.fromarray(rgb); s = long_edge / max(im.size)
    if s < 1: im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
    b = io.BytesIO(); im.save(b, 'JPEG', quality=quality)
    return base64.b64encode(b.getvalue()).decode()


class Server:
    def __init__(self, mid, backend='cuda', threads=None, port=None, ctx=8192, image_max_tokens=None, log=None):
        port = port or int(os.environ.get('ASSISTANT_PORT', 8091))
        d, m, p = MODELS[mid]
        self.mid, self.backend, self.port = mid, backend, port
        args = [BIN[backend], '-m', os.path.join(LLM, d, m), '--mmproj', os.path.join(LLM, d, p), '--port', str(port), '-c', str(ctx),
                '-np', '1', '--cache-ram', '0', '--reasoning', 'off', '--no-webui', '--seed', '7']
        if backend == 'cuda': args += ['-ngl', '999']
        else: args += ['-ngl', '0', '--no-mmproj-offload']
        if threads: args += ['-t', str(threads), '-tb', str(threads)]
        if image_max_tokens: args += ['--image-max-tokens', str(image_max_tokens)]
        self.log = open(log or os.path.join(ROOT, 'build', 'assistant', 'logs', f'server_{mid}_{backend}.log'), 'w', encoding='utf-8', errors='replace')
        t0 = time.time()
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT, cwd=os.path.dirname(BIN[backend]))
        while True:
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/health', timeout=2) as r:
                    if r.status == 200: break
            except Exception: pass
            if self.proc.poll() is not None: raise RuntimeError(f'llama-server {mid} exited; see log')
            if time.time() - t0 > 600: raise RuntimeError('server start timeout')
            time.sleep(0.5)
        self.load_s = time.time() - t0

    def rss_mb(self):
        try: return psutil.Process(self.proc.pid).memory_info().rss / 1e6
        except Exception: return None

    def chat(self, messages, schema=None, temperature=0.0, seed=7, max_tokens=400):
        body = {'messages': messages, 'temperature': temperature, 'seed': seed, 'max_tokens': max_tokens, 'cache_prompt': False}
        if temperature > 0: body.update(top_p=0.95, top_k=40)
        if schema is not None: body['response_format'] = {'type': 'json_schema', 'json_schema': {'name': 'apply_edit', 'schema': schema}}
        t0 = time.time()
        req = urllib.request.Request(f'http://127.0.0.1:{self.port}/v1/chat/completions', data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=900) as r: d = json.load(r)
        wall = time.time() - t0
        txt = d['choices'][0]['message'].get('content') or ''
        t = d.get('timings', {}); u = d.get('usage', {})
        return txt, dict(wall_s=wall, prompt_tokens=u.get('prompt_tokens'), completion_tokens=u.get('completion_tokens'),
                         prompt_ms=t.get('prompt_ms'), predicted_ms=t.get('predicted_ms'), prompt_tps=t.get('prompt_per_second'), gen_tps=t.get('predicted_per_second'))

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try: self.proc.wait(20)
            except Exception: self.proc.kill()
        self.log.close()


class Cloud:
    """OpenAI-compatible endpoint (arm E). Runs only when OPENAI_API_KEY is set. Only PUBLIC sample images may be sent."""
    def __init__(self, model=None):
        self.key = os.environ.get('OPENAI_API_KEY'); self.base = os.environ.get('OPENAI_BASE_URL', 'https://api.openai.com/v1')
        self.model = model or os.environ.get('ASSISTANT_CLOUD_MODEL', 'gpt-4.1-mini'); self.mid = 'cloud:' + self.model
        if not self.key: raise RuntimeError('OPENAI_API_KEY not set: arm E not run')

    def chat(self, messages, schema=None, temperature=0.0, seed=7, max_tokens=400):
        body = {'model': self.model, 'messages': messages, 'temperature': temperature, 'seed': seed, 'max_tokens': max_tokens}
        if schema is not None: body['response_format'] = {'type': 'json_schema', 'json_schema': {'name': 'apply_edit', 'schema': schema, 'strict': False}}
        t0 = time.time()
        req = urllib.request.Request(self.base.rstrip('/') + '/chat/completions', data=json.dumps(body).encode(),
                                     headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + self.key})
        with urllib.request.urlopen(req, timeout=300) as r: d = json.load(r)
        u = d.get('usage', {})
        return d['choices'][0]['message'].get('content') or '', dict(wall_s=time.time() - t0, prompt_tokens=u.get('prompt_tokens'), completion_tokens=u.get('completion_tokens'))

    def close(self): pass
    def rss_mb(self): return None
