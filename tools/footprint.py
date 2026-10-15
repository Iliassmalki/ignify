#!/usr/bin/env python3
"""Bloat check: run ignify headless against the mock library and report what it costs.

    python3 tools/footprint.py build/ignify [albums]      # default 300 albums

Measures binary size, time until the first player poll, memory once the library has loaded
(PSS counts shared libraries fairly; heap is ignify's own allocations) and CPU while playing.
Exits 1 when a budget is blown, so it can gate a change.
"""
import os, shutil, socket, subprocess, sys, tempfile, time

BUDGET = {"binary_kb": 1024, "ready_s": 3.0, "pss_mb": 100.0, "heap_mb": 40.0, "cpu_pct": 3.0}
HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def mem(pid):
    pss = heap = 0
    name = ""
    with open(f"/proc/{pid}/smaps") as f:
        for line in f:
            first = line.split()[0]
            if "-" in first and ":" not in first:  # mapping header
                parts = line.split()
                name = parts[5] if len(parts) > 5 else ""
            elif first == "Pss:":
                kb = int(line.split()[1])
                pss += kb
                heap += kb if name == "[heap]" else 0
    return pss / 1024, heap / 1024


def cpu_ticks(pid):
    fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def main():
    binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/ignify")
    albums = sys.argv[2] if len(sys.argv) > 2 else "300"
    tmp = tempfile.mkdtemp(prefix="ignify-footprint-")
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    try:
        os.makedirs(f"{tmp}/config/ignify")
        os.makedirs(f"{tmp}/bin")
        with open(f"{tmp}/config/ignify/ignify.conf", "w") as f:
            f.write("[General]\nclientId=0123456789abcdef0123456789abcdef\nrefreshToken=mock\n")
        for stub in ("librespot", "xdg-open"):  # no real audio engine, never a real browser
            with open(f"{tmp}/bin/{stub}", "w") as f:
                f.write("#!/bin/sh\nexec sleep 600\n" if stub == "librespot" else "#!/bin/sh\nexit 0\n")
            os.chmod(f"{tmp}/bin/{stub}", 0o755)

        mock = subprocess.Popen([sys.executable, "-I", f"{HERE}/mock_spotify.py"], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True,
                                env={**os.environ, "MOCK_PORT": str(port), "MOCK_ALBUMS": albums})
        time.sleep(1.5)  # mock renders its cover PNGs at start
        env = {**os.environ, "QT_QPA_PLATFORM": "offscreen", "XDG_CONFIG_HOME": f"{tmp}/config",
               "XDG_CACHE_HOME": f"{tmp}/cache", "PATH": f"{tmp}/bin:" + os.environ["PATH"],
               "BROWSER": f"{tmp}/bin/xdg-open", "IGNIFY_API": f"{base}/v1", "IGNIFY_ACCOUNTS": base,
               "IGNIFY_LYRICS": f"{base}/lrclib", "IGNIFY_WIKI": f"{base}/wiki"}
        t0 = time.time()
        app = subprocess.Popen([binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for line in mock.stdout:
            if line.strip() == "READY":
                break
        ready = time.time() - t0
        time.sleep(8)  # album pages and visible covers load
        pss, heap = mem(app.pid)
        t1, c1 = time.time(), cpu_ticks(app.pid)
        time.sleep(5)
        cpu = (cpu_ticks(app.pid) - c1) / os.sysconf("SC_CLK_TCK") / (time.time() - t1) * 100
        app.terminate()
        mock.terminate()
        app.wait(5)
        mock.wait(5)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    got = {"binary_kb": os.path.getsize(binary) / 1024, "ready_s": ready, "pss_mb": pss, "heap_mb": heap,
           "cpu_pct": cpu}
    print(f"ignify footprint, {albums}-album library (headless)")
    failed = False
    for key, limit in BUDGET.items():
        ok = got[key] <= limit
        failed |= not ok
        print(f"  {key:10} {got[key]:8.1f}   budget {limit:8.1f}   {'ok' if ok else 'OVER'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
