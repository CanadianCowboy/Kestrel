"""Local Kokoro text-to-speech for Kestrel. Not part of the build; see tools/.

Reads one JSON request per line on stdin and writes one WAV per request. The
model is loaded once, at startup, and stays loaded: Kestrel speaks a reply one
clause at a time, and paying the load cost per clause would put a second of
silence between every sentence.

    {"text": "On it.", "out": "C:/tmp/clause-1.wav", "voice": "bm_george"}
    -> {"ok": true, "out": "...", "seconds": 1.2, "sample_rate": 24000}
    -> {"ok": false, "error": "..."}

Voice is optional and defaults to the configured one. "speed" scales with the
persona: 1.0 is the model's own pace, and the caller sends the same rate the
voice session already computed for the clause.
"""

import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The model is a third-party download, so it lives beside the interpreter that
# runs it rather than in the repository. Checked in the order a developer is
# most likely to have put it.
MODEL_DIRS = [
    os.environ.get("KESTREL_VOICE_MODEL_DIR", ""),
    os.path.join(REPO, ".kestrel-voice", "models"),
    os.path.join(REPO, "tools", "models"),
]
DEFAULT_VOICE = os.environ.get("KESTREL_VOICE", "bm_george")


def find(name):
    for directory in MODEL_DIRS:
        if not directory:
            continue
        candidate = os.path.join(directory, name)
        if os.path.exists(candidate):
            return candidate
    return os.path.join(MODEL_DIRS[1] or ".", name)


MODEL = find("kokoro-v1.0.onnx")
VOICES = find("voices-v1.0.bin")


def fail(message):
    sys.stdout.write(json.dumps({"ok": False, "error": message}) + "\n")
    sys.stdout.flush()


def add_cuda_dll_directories():
    """Puts the CUDA libraries pip installed on the DLL search path.

    The wheels for cuDNN, cuBLAS and nvrtc land in `site-packages/nvidia/*/bin`
    and are never added to the process search path by pip, because nothing
    declares that they are load-time dependencies of a general Python program.
    Without this, importing onnxruntime-gpu succeeds, CUDAExecutionProvider is
    listed as available, and the first session creation dies inside the provider
    with a load error for a DLL that is present on disk -- the same shape of
    failure as the missing platform plugin that made a Qt dialog hang forever.

    Returns the directories added, for the startup report.
    """
    import glob
    import sys

    site = os.path.join(os.path.dirname(os.path.abspath(sys.executable)),
                        "..", "Lib", "site-packages", "nvidia")
    added = []
    for package in ("cudnn", "cublas", "cuda_nvrtc", "cuda_runtime"):
        directory = os.path.abspath(os.path.join(site, package, "bin"))
        if not os.path.isdir(directory):
            continue
        try:
            os.add_dll_directory(directory)
        except (OSError, AttributeError):
            # add_dll_directory is Windows-only and 3.8+. Neither absence is a
            # reason to fail: on a machine where the libraries are already on
            # PATH there is nothing to add, and the session attempt below is the
            # real test of whether the GPU path works.
            continue
        added.append(directory)
    return added


def cudnn_usable():
    """Whether the CUDA execution provider can actually get a cuDNN handle.

    This has to run in a child process, and that is the whole point of it.

    When cuDNN is present but incomplete, ONNX Runtime does not raise. It prints
    "Invalid handle. Cannot load symbol cudnnCreate" to stderr and terminates
    the process -- an abort in native code, which no `except` clause in this
    process can intercept. An in-process try/except around session creation is
    therefore not a fallback, it is a hope: the voice server disappears between
    clauses and takes the persona's speech with it. The only thing that actually
    works is to find out in a process we are willing to lose.

    cuDNN's Windows binaries delay-load `zlibwapi.dll`, which neither the
    onnxruntime-gpu wheel nor the cuDNN wheel carries. A missing one leaves
    ctypes.WinDLL("cudnn64_9.dll") working -- the shim resolves, cudnnCreate
    resolves -- while the first real call inside the provider dies. So the check
    is the call, not the load.

    Returns (ok, detail) for the ready line.
    """
    import subprocess

    # add_dll_directory registrations do not survive into a child process, so
    # the child has to make the same ones. Pointing it at this interpreter's
    # own site-packages is what keeps the probe honest on a machine where the
    # libraries were pip-installed rather than put on PATH by the toolkit.
    probe = (
        "import ctypes,os,sys\n"
        "site=os.path.join(os.path.dirname(os.path.abspath(sys.executable)),"
        "'..','Lib','site-packages','nvidia')\n"
        "for pkg in ('cudnn','cublas','cuda_nvrtc','cuda_runtime'):\n"
        "    d=os.path.abspath(os.path.join(site,pkg,'bin'))\n"
        "    if os.path.isdir(d) and hasattr(os,'add_dll_directory'):"
        " os.add_dll_directory(d)\n"
        "h=ctypes.c_size_t(0)\n"
        "lib=ctypes.WinDLL('cudnn64_9.dll')\n"
        "lib.cudnnCreate.argtypes=[ctypes.POINTER(ctypes.c_size_t)]\n"
        "lib.cudnnCreate.restype=ctypes.c_int\n"
        "rc=lib.cudnnCreate(ctypes.byref(h))\n"
        "lib.cudnnDestroy.argtypes=[ctypes.c_size_t]\n"
        "lib.cudnnDestroy(ctypes.c_size_t(h.value))\n"
        "sys.exit(0 if rc==0 else 3)\n"
    )
    try:
        done = subprocess.run([sys.executable, "-c", probe],
                              capture_output=True, text=True, timeout=60)
    except Exception as error:  # noqa: BLE001 - reported, not swallowed
        return False, "cuDNN probe failed (%s)" % error
    if done.returncode == 0:
        return True, ""
    detail = ""
    for line in (done.stderr or "").splitlines():
        if "Cannot load symbol" in line or "Invalid handle" in line:
            detail = line.strip()[-60:]
            break
    return False, detail or "cuDNN probe exited %d" % done.returncode


def session_options(rt):
    """Session options that stop ONNX Runtime from using every core.

    The default is `intra_op_num_threads = 0`, which means "one thread per
    core". On a 16-core machine that is the worst setting available here, and
    not by a small margin: measured over a seven-clause reply of the shape
    Kestrel actually speaks (clause at a time, mixed lengths), synthesising
    18.4s of audio took 24.4s on all 16 threads and 15.1s on four. The first
    number is the one that matters most -- it is six seconds *slower than real
    time*, so every clause is delivered after the sentence before it has
    finished and the voice falls steadily further behind the text.

    Four threads also removes a cliff that is easy to misread as a bug: at the
    default the first clause is synthesised in 0.4s and every one after it takes
    7s or more, which looks exactly like a warm-up problem in the wrong
    direction. It is the arena, not warm-up. Four threads is flat across calls.

    Kokoro is 82M parameters and the per-op work is small, so past a handful of
    threads the spin-wait and the memory traffic cost more than the extra cores
    return. This process is also launched by the app, which already has a QML
    scene, a llama.cpp generation worker and a CUDA context competing for the
    same machine; taking all sixteen would be taking them from the rest of
    Kestrel as well.
    """
    options = rt.SessionOptions()
    try:
        threads = int(os.environ.get("KESTREL_VOICE_THREADS", "4"))
    except ValueError:
        threads = 4
    options.intra_op_num_threads = max(1, threads)
    # The graph is one straight-line decoder. Inter-op scheduling across
    # parallel branches buys nothing here and costs a thread hand-off per run.
    options.inter_op_num_threads = 1
    return options


def build_session(kokoro_module, model_path, voices_path):
    """Loads Kokoro on the GPU when that works, and on the CPU when it does not.

    kokoro-onnx picks its own provider: all available ones when the GPU package
    is installed, the CPU otherwise. That is a reasonable default and not
    enough here, for two reasons.

    The first is that "the GPU package is installed" is not "the GPU works". The
    CUDA provider still needs a loadable cuDNN, matching cuBLAS and a working
    driver, and when any of those is missing it does not fail politely -- see
    cudnn_usable(). The GPU is therefore only requested when the pre-flight
    passed, and a request can always be refused with KESTREL_VOICE_PROVIDER.

    The second is that nothing reports which provider actually ran. The reply to
    a clause is the same either way, so a silent fall-back to the CPU is
    invisible: the app sounds fine and the GPU sits idle. The chosen provider is
    therefore part of the ready line, where it can be seen.
    """
    from kokoro_onnx import Kokoro

    add_cuda_dll_directories()

    import onnxruntime as rt

    available = list(rt.get_available_providers())
    wanted = os.environ.get("KESTREL_VOICE_PROVIDER", "auto").strip().lower()
    notes = []

    if wanted == "cpu":
        session = rt.InferenceSession(model_path, sess_options=session_options(rt),
                                      providers=["CPUExecutionProvider"])
        return Kokoro.from_session(session, voices_path), "CPU (requested)"

    if "CUDAExecutionProvider" in available and wanted != "cpu":
        ok, detail = cudnn_usable()
        if ok:
            session = rt.InferenceSession(
                model_path,
                sess_options=session_options(rt),
                providers=["CUDAExecutionProvider", "CPUExecutionProvider"])
            # Trust the session, not the request: ONNX Runtime will fall back to
            # the CPU silently if a node cannot be placed on the GPU, and that
            # is the case worth knowing about.
            placed = session.get_providers()
            notes.append("requested CUDA")
            if placed and placed[0] == "CPUExecutionProvider":
                notes.append("but ran on the CPU")
            return Kokoro.from_session(session, voices_path), "; ".join(notes)
        notes.append("CUDA skipped (%s)" % (detail or "cuDNN unusable"))
    elif wanted not in ("auto", "gpu"):
        notes.append("CUDA not available")

    session = rt.InferenceSession(model_path, sess_options=session_options(rt),
                                  providers=["CPUExecutionProvider"])
    return Kokoro.from_session(session, voices_path), "; ".join(
        notes + ["ran on the CPU"])


def main():
    try:
        import numpy as np
        import soundfile as sf
    except Exception as error:  # noqa: BLE001 - reported, not swallowed
        fail("kokoro is not importable: %s" % error)
        return 1

    for path in (MODEL, VOICES):
        if not os.path.exists(path):
            fail("missing model file: %s" % path)
            return 1

    try:
        kokoro, provider_note = build_session(None, MODEL, VOICES)
    except Exception as error:  # noqa: BLE001 - reported, not swallowed
        fail("kokoro failed to load: %s" % error)
        return 1

    # A ready signal, so Kestrel can tell "still starting up" from "broken"
    # instead of guessing from a missing first reply.
    #
    # "provider" is in it because the difference is otherwise undetectable: a
    # clause sounds the same whether the GPU or the CPU produced it, so a silent
    # fall-back leaves nobody any way of knowing the 4060 is doing nothing.
    sys.stdout.write(
        json.dumps({"ok": True, "ready": True, "voice": DEFAULT_VOICE,
                    "provider": provider_note}) + "\n"
    )
    sys.stdout.flush()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except ValueError as error:
            fail("bad request: %s" % error)
            continue

        text = request.get("text", "")
        if not text.strip():
            fail("no text to speak")
            continue

        try:
            samples, sample_rate = kokoro.create(
                text,
                voice=request.get("voice") or DEFAULT_VOICE,
                speed=float(request.get("speed", 1.0)),
                lang=request.get("lang", "en-gb"),
            )
            out = request["out"]
            parent = os.path.dirname(out)
            if parent:
                os.makedirs(parent, exist_ok=True)
            sf.write(out, np.asarray(samples, dtype="float32"), sample_rate)
            sys.stdout.write(
                json.dumps(
                    {
                        "ok": True,
                        "out": out,
                        "sample_rate": sample_rate,
                        "seconds": round(len(samples) / float(sample_rate), 3),
                    }
                )
                + "\n"
            )
            sys.stdout.flush()
        except Exception as error:  # noqa: BLE001 - reported, not swallowed
            fail("synthesis failed: %s" % error)
    return 0


if __name__ == "__main__":
    sys.exit(main())
