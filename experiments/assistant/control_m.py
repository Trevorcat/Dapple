"""Control arm M (no model): the auto plus a constant modern_clean base at 70 % (what the VLMs chose for most photos), through the same
guards. Separates 'the model's judgement' from 'a fixed base everybody gets'. python -I experiments/assistant/control_m.py"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assist, data, run, tools

gen, five = run.sets()
for path in gen + five:
    iid = data.image_id(path)
    if iid in run.done_ids(os.path.join(run.RES, 'engine', 'M_clean70.jsonl')): continue
    P = data.Prepared(path)
    e = tools.empty_edit(); e['base'] = 'modern_clean'; e['base_strength'] = 7
    img, m, scale, ev = assist.guard(P, e)
    run.save('engine', 'M_clean70', dict(id=iid, edit=e, eff=assist.effective(e, scale), m=m, guards=ev), img)
    P.close(); print('M', iid, flush=True)
