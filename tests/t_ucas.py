#!/usr/bin/env python3
"""ucas built into vxide: the multi-call entry, the JSON frontend the ucas tab
talks to, and the cancel patch (SIGUSR1 to the process group stops a request
under timeout_s and keeps the session's state).

    VXIDE=build/vxide python3 tests/t_ucas.py
"""
import json, os, shutil, signal, subprocess, sys, time

exe = os.environ.get("VXIDE", "build/vxide")
fails = 0


def check(what, ok, detail=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else "  " + detail))
    if not ok:
        fails += 1


# the command line, through `vxide --ucas`
out = subprocess.run([exe, "--ucas", "-e", "range (x: u8) / 3"], capture_output=True, text=True, timeout=60).stdout
check("vxide --ucas runs a statement", "0..85" in out, out)

# the JSON frontend, as the tab starts it (argv[0] "ucas", its own group)
p = subprocess.Popen(["ucas", "--json", "--files", "base64"], executable=exe, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, text=True, start_new_session=True)


def ask(rq):
    p.stdin.write(json.dumps(rq) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


r = ask({"id": 1, "cmd": "ucas", "args": {"source": "g(x: u32) = x * 7", "timeout_s": 600}})
check("define under timeout_s", r.get("ok") is True, str(r))
r = ask({"id": 2, "cmd": "ucas", "args": {"source": "plot sin x", "timeout_s": 600}})
f = (r.get("files") or [{}])[0]
check("plot answers a GIF inline", f.get("mime") == "image/gif" and f.get("base64", "").startswith("R0lGOD"), str(r)[:300])

# a long factorisation (RSA-100), cancelled
rsa100 = ("15226050279225333605356183781326374297180681149613806886579084945801229632589528976540003506920"
          "06139")
p.stdin.write(json.dumps({"id": 3, "cmd": "ucas", "args": {"source": "q(x: u8) = x + 1\nbigint factor " + rsa100,
                                                           "timeout_s": 600}}) + "\n")
p.stdin.flush()
time.sleep(1.0)
t0 = time.time()
os.killpg(p.pid, signal.SIGUSR1)
r = json.loads(p.stdout.readline())
check("SIGUSR1 cancels the running request", r.get("id") == 3 and (r.get("error") or {}).get("code") == "cancelled",
      str(r)[:300])
check("cancel answers at once", time.time() - t0 < 2.0, "%.2f s" % (time.time() - t0))
r = ask({"id": 4, "cmd": "state", "args": {"timeout_s": 600}})
names = [fn.get("name") for fn in (r.get("result") or {}).get("functions", [])]
check("the state survives a cancel", "g" in names and "q" not in names, str(names))
# code blocks, as the tab sends them: a statement and its block through the
# text frontend (the english command), the block's language given
c = "unsigned f(unsigned x) { return (x * 8) / 4; }"
r = ask({"id": 41, "cmd": "english", "args": {"text": "c optimize nofile", "code": c, "lang": "c", "timeout_s": 600}})
check("a ```c block: c optimize", r.get("ok") is True and "1 rewrite proved" in r.get("text", ""), str(r)[:300])
if shutil.which("glslangValidator") and shutil.which("spirv-opt"):
    g = ("#version 450\nlayout(local_size_x = 1) in;\nlayout(std430, binding = 0) buffer B { uint v[]; };\n"
         "void main() { uint x = v[0] & 255u; v[1] = min(x, 300u); }")
    r = ask({"id": 42, "cmd": "english", "args": {"text": "simplify", "code": g, "lang": "glsl", "timeout_s": 600}})
    names = [f.get("name") for f in r.get("files", [])]
    check("a ```glsl block: simplify, proved by SAT", r.get("ok") is True and "simplified.spvasm" in names and
          "proved by SAT" in r.get("text", ""), str(r)[:300])
else:
    print("skip ```glsl block (no glslangValidator / spirv-opt)")
# adamfit (cmake/ucas-adamfit.patch): candidates certified, and its progress
# on stderr while the answer is pending (what the tab shows live)
q = subprocess.run(["ucas", "--json", "--files", "none"], executable=exe, capture_output=True, text=True, timeout=120,
                   input=json.dumps({"id": 1, "cmd": "adamfit", "args": {"f": "cos(x)", "in": [0, 1],
                                                                           "options": "count 120 steps 30 list 2"}}) + "\n")
r = json.loads(q.stdout.splitlines()[0])
cands = (r.get("result") or {}).get("candidates", [])
check("adamfit answers certified candidates", r.get("ok") is True and len(cands) == 2 and cands[0].get("certified")
      and cands[0]["score"] <= cands[1]["score"], str(r)[:300])
check("adamfit's progress goes to stderr", "ucas: adamfit:   [" in q.stderr and "ucas: adamfit:   ..." in q.stderr,
      q.stderr[:300])
r = ask({"id": 5, "cmd": "reset", "args": {}})
r = ask({"id": 6, "cmd": "state", "args": {}})
check("reset re-executes as ucas", r.get("ok") is True and not (r.get("result") or {}).get("functions"), str(r)[:300])
p.stdin.close()
p.wait(timeout=30)
check("exits when stdin closes", p.returncode == 0, str(p.returncode))
sys.exit(1 if fails else 0)
